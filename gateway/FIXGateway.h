#pragma once
#include "OrderEvent.h"
#include "EventLog.h"
#include "PositionTracker.h"
#include "OrderManager.h"
#include "RiskChecker.h"
#include "MessageTranslator.h"
#include "Killswitch.h"

#include <quickfix/Application.h>
#include <quickfix/MessageCracker.h>
#include <quickfix/SessionID.h>
#include <quickfix/fix42/ExecutionReport.h>
#include <quickfix/fix42/NewOrderSingle.h>
#include <quickfix/fix42/OrderCancelRequest.h>
#include <quickfix/fix42/Logon.h>

#include <atomic>
#include <cstdint>
#include <string_view>

namespace hft::gateway {

// FIXGateway
// -----------
// QuickFIX Application subclass that bridges the exchange session to the
// internal bookkeeping layer. Owns no state of its own - it is a thin
// adapter between QuickFIX callbacks and your EventLog / PositionTracker /
// OrderManager / RiskChecker.
//
// Thread model:
//   QuickFIX calls fromApp() on its own internal I/O thread.
//   send_order() and cancel_all() are called from the strategy thread.
//   The bookkeeping components (EventLog, PositionTracker, OrderManager)
//   must therefore be protected or — as in this design — the gateway
//   serialises all writes through a single writer thread pattern.
//   For a first implementation, calling all three from the same thread
//   (QuickFIX's I/O thread via fromApp) is safe and simplest.
//
// Sequence:
//   1. QuickFIX establishes session, calls onLogon()
//   2. Strategy calls send_order() → risk check → NewOrderSingle sent
//   3. Exchange responds with ExecutionReport → fromApp() → onMessage()
//   4. onMessage() translates → appends to EventLog → updates trackers
//   5. On shutdown or error → Killswitch::execute() → cancel_all()

class FIXGateway : public FIX::Application,
                   public FIX::MessageCracker
{
public:
    FIXGateway(EventLog&        log,
               PositionTracker& tracker,
               OrderManager&    orders,
               RiskChecker&     risk);

    ~FIXGateway() override = default;

    FIXGateway(const FIXGateway&)            = delete;
    FIXGateway& operator=(const FIXGateway&) = delete;

    //      outbound API (called by strategy thread)

    // Run a risk check then send a NewOrderSingle if it passes.
    // Returns the assigned order_id on success, empty string on risk block
    // or if the session is not connected.
    [[nodiscard]] std::string send_order(std::string_view symbol,
                                          Side             side,
                                          int32_t          qty,
                                          int64_t          price_fp);

    // Convenience overload accepting a double price for readability.
    [[nodiscard]] std::string send_order(std::string_view symbol,
                                          Side             side,
                                          int32_t          qty,
                                          double           price);

    // Trigger the killswitch: cancel all open orders.
    void emergency_cancel_all();

    //      session state

    [[nodiscard]] bool is_connected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const FIX::SessionID& session_id() const noexcept {
        return session_id_;
    }

    //      FIX::Application interface

    void onCreate(const FIX::SessionID&)                              override;
    void onLogon(const FIX::SessionID&)                               override;
    void onLogout(const FIX::SessionID&)                              override;
    void toAdmin(FIX::Message&, const FIX::SessionID&)                override;
    void toApp(FIX::Message&, const FIX::SessionID&)      noexcept    override;
    void fromAdmin(const FIX::Message&,
                   const FIX::SessionID&)                 noexcept    override;
    void fromApp(const FIX::Message&,
                 const FIX::SessionID&)                   noexcept    override;

    //      FIX::MessageCracker handlers

    void onMessage(const FIX42::ExecutionReport&,
                   const FIX::SessionID&)                 noexcept    override;

private:
    [[nodiscard]] std::string next_order_id();

    // Bookkeeping components — non-owning references
    EventLog&        log_;
    PositionTracker& tracker_;
    OrderManager&    orders_;
    RiskChecker&     risk_;

    FIX::SessionID    session_id_;
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> order_counter_{0};
};

} // namespace hft::gateway
