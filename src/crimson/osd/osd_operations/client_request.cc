// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab expandtab

#include <seastar/core/future.hh>

#include "messages/MOSDOp.h"
#include "messages/MOSDOpReply.h"

#include "crimson/common/exception.h"
#include "crimson/osd/pg.h"
#include "crimson/osd/osd.h"
#include "common/Formatter.h"
#include "crimson/osd/osd_operations/client_request.h"
#include "crimson/osd/osd_connection_priv.h"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_osd);
  }
}

namespace crimson::osd {

ClientRequest::ClientRequest(
  OSD &osd, crimson::net::ConnectionRef conn, Ref<MOSDOp> &&m)
  : osd(osd),
    conn(conn),
    m(m),
    sequencer(get_osd_priv(conn.get()).op_sequencer[m->get_spg()]),
    prev_op_id(sequencer.get_last_issued())
{}

ClientRequest::~ClientRequest()
{
  logger().debug("{}: destroying", *this);
}

void ClientRequest::print(std::ostream &lhs) const
{
  lhs << *m;
}

void ClientRequest::dump_detail(Formatter *f) const
{
}

ClientRequest::ConnectionPipeline &ClientRequest::cp()
{
  return get_osd_priv(conn.get()).client_request_conn_pipeline;
}

ClientRequest::PGPipeline &ClientRequest::pp(PG &pg)
{
  return pg.client_request_pg_pipeline;
}

bool ClientRequest::is_pg_op() const
{
  return std::any_of(
    begin(m->ops), end(m->ops),
    [](auto& op) { return ceph_osd_op_type_pg(op.op.op); });
}

seastar::future<> ClientRequest::start()
{
  logger().debug("{}: start", *this);

  return crimson::common::handle_system_shutdown(
    [this, opref=IRef{this}]() mutable {
    return seastar::repeat([this, opref=std::move(opref)]() mutable {
      logger().debug("{}: in repeat", *this);
      return with_blocking_future(handle.enter(cp().await_map))
      .then([this] {
	return with_blocking_future(osd.osdmap_gate.wait_for_map(m->get_min_epoch()));
      }).then([this](epoch_t epoch) {
	return with_blocking_future(handle.enter(cp().get_pg));
      }).then([this] {
	return with_blocking_future(osd.wait_for_pg(m->get_spg()));
      }).then([this](Ref<PG> pgref) mutable {
	epoch_t same_interval_since = pgref->get_interval_start_epoch();
	logger().debug("{} same_interval_since: {}", *this, same_interval_since);
	return sequencer.start_op(
	  handle, prev_op_id, get_id(),
	  [this, pgref] {
	  PG &pg = *pgref;
	  if (pg.can_discard_op(*m)) {
	    logger().debug("{} op discarded, {}, same_primary_since: {}",
		*this, pg, pg.get_info().history.same_primary_since);
	    return osd.send_incremental_map(conn, m->get_map_epoch());
	  }
	  return with_blocking_future(
	    handle.enter(pp(pg).await_map)
	  ).then([this, &pg]() mutable {
	    return with_blocking_future(
	      pg.osdmap_gate.wait_for_map(m->get_min_epoch()));
	  }).then([this, &pg](auto map) mutable {
	    return with_blocking_future(
	      handle.enter(pp(pg).wait_for_active));
	  }).then([this, &pg]() mutable {
	    return with_blocking_future(pg.wait_for_active_blocker.wait());
	  }).then([this, pgref=std::move(pgref)]() mutable {
	    if (m->finish_decode()) {
	      m->clear_payload();
	    }
	    if (is_pg_op()) {
	      return process_pg_op(pgref);
	    } else {
	      return process_op(pgref);
	    }
	  });
        }).then([this] {
          sequencer.finish_op(get_id());
          return seastar::stop_iteration::yes;
        }).handle_exception_type(
          [this](crimson::common::actingset_changed& e) mutable {
          if (e.is_primary()) {
            logger().debug("operation restart, acting set changed");
            sequencer.maybe_reset(get_id());
            return seastar::stop_iteration::no;
          } else {
            sequencer.abort();
            logger().debug("operation abort, up primary changed");
            return seastar::stop_iteration::yes;
          }
        }).handle_exception_type(
          [](seastar::broken_condition_variable&) {
          return seastar::stop_iteration::yes;
        });
      });
    });
  });
}

seastar::future<> ClientRequest::process_pg_op(
  Ref<PG> &pg)
{
  return pg->do_pg_ops(m)
    .then([this, pg=std::move(pg)](Ref<MOSDOpReply> reply) {
      return conn->send(reply);
    });
}

seastar::future<> ClientRequest::process_op(Ref<PG> &pg)
{
  return with_blocking_future(handle.enter(pp(*pg).recover_missing)).then(
    [this, pg]() mutable {
    return do_recover_missing(pg);
  }).then([this, pg]() mutable {
    return pg->already_complete(m->get_reqid()).then_unpack(
      [this, pg](bool completed, int ret) mutable
      -> PG::load_obc_ertr::future<> {
      if (completed) {
        auto reply = make_message<MOSDOpReply>(
          m.get(), ret, pg->get_osdmap_epoch(),
          CEPH_OSD_FLAG_ACK | CEPH_OSD_FLAG_ONDISK, false);
        return conn->send(std::move(reply));
      } else {
        return with_blocking_future(handle.enter(pp(*pg).get_obc)).then(
          [this, pg]() mutable -> PG::load_obc_ertr::future<> {
          logger().debug("{}: got obc lock", *this);
          op_info.set_from_op(&*m, *pg->get_osdmap());
          return pg->with_locked_obc(m, op_info, this, [this, pg](auto obc) mutable {
            return with_blocking_future(handle.enter(pp(*pg).process)).then(
              [this, pg, obc]() mutable {
              return do_process(pg, obc);
            });
          });
        });
      }
    });
  }).safe_then([pg=std::move(pg)] {
    return seastar::now();
  }, PG::load_obc_ertr::all_same_way([](auto &code) {
    logger().error("ClientRequest saw error code {}", code);
    return seastar::now();
  }));
}

seastar::future<> ClientRequest::do_recover_missing(Ref<PG>& pg)
{
  eversion_t ver;
  const hobject_t& soid = m->get_hobj();
  logger().debug("{} check for recovery, {}", *this, soid);
  if (!pg->is_unreadable_object(soid, &ver) &&
      !pg->is_degraded_or_backfilling_object(soid)) {
    return seastar::now();
  }
  logger().debug("{} need to wait for recovery, {}", *this, soid);
  if (pg->get_recovery_backend()->is_recovering(soid)) {
    return pg->get_recovery_backend()->get_recovering(soid).wait_for_recovered();
  } else {
    auto [op, fut] =
      osd.get_shard_services().start_operation<UrgentRecovery>(
        soid, ver, pg, osd.get_shard_services(), pg->get_osdmap_epoch());
    return std::move(fut);
  }
}

seastar::future<>
ClientRequest::do_process(Ref<PG>& pg, crimson::osd::ObjectContextRef obc)
{
  if (!pg->is_primary()) {
    // primary can handle both normal ops and balanced reads
    if (is_misdirected(*pg)) {
      logger().trace("process_op: dropping misdirected op");
      return seastar::now();
    } else if (const hobject_t& hoid = m->get_hobj();
               !pg->get_peering_state().can_serve_replica_read(hoid)) {
      auto reply = make_message<MOSDOpReply>(
	m.get(), -EAGAIN, pg->get_osdmap_epoch(),
	m->get_flags() & (CEPH_OSD_FLAG_ACK|CEPH_OSD_FLAG_ONDISK),
	!m->has_flag(CEPH_OSD_FLAG_RETURNVEC));
      return conn->send(std::move(reply));
    }
  }
  return pg->do_osd_ops(m, obc, op_info).safe_then([this](Ref<MOSDOpReply> reply) {
    return conn->send(std::move(reply));
  }, crimson::ct_error::eagain::handle([this, pg]() mutable {
    return process_op(pg);
  }));
}

bool ClientRequest::is_misdirected(const PG& pg) const
{
  // otherwise take a closer look
  if (const int flags = m->get_flags();
      flags & CEPH_OSD_FLAG_BALANCE_READS ||
      flags & CEPH_OSD_FLAG_LOCALIZE_READS) {
    if (!op_info.may_read()) {
      // no read found, so it can't be balanced read
      return true;
    }
    if (op_info.may_write() || op_info.may_cache()) {
      // write op, but i am not primary
      return true;
    }
    // balanced reads; any replica will do
    return pg.is_nonprimary();
  }
  // neither balanced nor localize reads
  return true;
}

}
