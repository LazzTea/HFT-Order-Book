#include "FIXGateway.h"

#include <quickfix/Session.h>

#include <cstdio>
#include <string>

namespace hft::gateway {

FIXGateway::FIXGateway(EventLog&        log,
                       PositionTracker& tracker,
                       OrderManager&    orders,
                       RiskChecker&     risk)
    : log_(log), tracker_(tracker), orders_(orders), risk_(risk)
{}

// ── FIX::Application callbacks ────────────────────────────────────────────────

void FIXGateway::onCreate(const FIX::SessionID& id) {
    session_id_ = id;
    printf("[FIX] session created: %s\n", id.toString().c_str());
}

void FIXGateway::onLogon(const FIX::SessionID& id) {
    connected_.store(true, std::memory_order_release);
    printf("[FIX] logon: %s\n", id.toString().c_str());
}

void FIXGateway::onLogout(const FIX::SessionID& id) {
    connected_.store(false, std::memory_order_release);
    printf("[FIX] logout: %s\n", id.toString().c_str());
}

void FIXGateway::toAdmin(FIX::Message&, const FIX::SessionID&) {
    // Override here to inject credentials (e.g. Username/Password tags
    // on the Logon message) if the broker's session requires them.
}

void FIXGateway::toApp(FIX::Message&, const FIX::SessionID&)
    noexcept
{
    // Called before every outbound application message.
    // Use for audit logging or last-chance field injection.
}

void FIXGateway::fromAdmin(const FIX::Message&, const FIX::SessionID&)
    noexcept
{
    // Heartbeats, TestRequest, ResendRequest, SequenceReset all arrive here.
    // QuickFIX handles sequence management automatically — nothing to do.
}

void FIXGateway::fromApp(const FIX::Message&  msg,
                          const FIX::SessionID& id)
    noexcept
{
    // MessageCracker routes by MsgType to the correct onMessage() overload.
    crack(msg, id);
}

// ── Inbound message handlers ──────────────────────────────────────────────────

void FIXGateway::onMessage(const FIX42::ExecutionReport& report,
                            const FIX::SessionID&)
    noexcept
{
    const auto maybe_evt = MessageTranslator::from_execution_report(report);
    if (!maybe_evt.has_value()) return;  // ack-only, nothing to book

    const OrderEvent& evt = maybe_evt.value();

    // The three bookkeeping calls — same pattern as your synthetic tests.
    // All three are called on QuickFIX's I/O thread, which is the single
    // writer for this design. If you later need the strategy on a separate
    // thread, interpose a ring buffer here.
    log_.append(evt);
    tracker_.apply_event(evt);
    orders_.apply_event(evt);

    // Debug trace — replace with structured logging in production
    printf("[FIX] %s %s %s qty=%d px=%.4f\n",
           to_string(evt.type == EventType::PartialFill ? OrderState::PartiallyFilled :
                     evt.type == EventType::FullFill    ? OrderState::Filled          :
                     evt.type == EventType::Cancel      ? OrderState::Cancelled        :
                                                          OrderState::Rejected),
           evt.side == Side::Buy ? "BUY" : "SELL",
           evt.symbol_view().data(),
           evt.fill_qty,
           MessageTranslator::from_fixed(evt.fill_price));
}

// ── Outbound order sending ────────────────────────────────────────────────────

std::string FIXGateway::send_order(const std::string_view symbol,
                                    const Side             side,
                                    const int32_t          qty,
                                    const int64_t          price_fp)
{
    if (!is_connected()) {
        printf("[FIX] send_order blocked: not connected\n");
        return {};
    }

    const auto result = risk_.check(symbol, side, qty, price_fp,
                                     tracker_, orders_);
    if (result != RiskResult::Pass) {
        printf("[RISK] order blocked: %s\n", to_string(result));
        return {};
    }

    const std::string oid = next_order_id();
    const auto evt = make_new_order(symbol, side, qty, price_fp, oid);

    // Record intention before sending — if we crash between send and ack,
    // the NewOrder event in the log shows the order was submitted.
    log_.append(evt);
    orders_.apply_event(evt);

    // FIX::Message& fix_order = MessageTranslator::to_new_order_single(evt);
    // FIX::Session::sendToTarget(fix_order, session_id_);

    auto fix_order = static_cast<FIX::Message>(MessageTranslator::to_new_order_single(evt));
    FIX::Session::sendToTarget(fix_order, session_id_);

    return oid;
}

std::string FIXGateway::send_order(const std::string_view symbol,
                                    const Side             side,
                                    const int32_t          qty,
                                    const double           price)
{
    return send_order(symbol, side, qty,
                      MessageTranslator::to_fixed(price));
}

void FIXGateway::emergency_cancel_all() {
    printf("[FIX] emergency_cancel_all triggered\n");
    Killswitch ks(orders_, tracker_, session_id_);
    ks.execute();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string FIXGateway::next_order_id() {
    return "ORD" + std::to_string(
        order_counter_.fetch_add(1, std::memory_order_relaxed));
}

} // namespace hft::gateway
