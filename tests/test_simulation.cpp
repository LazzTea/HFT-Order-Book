#include "../OrderEvent.h"
#include "../EventLog.h"
#include "../PositionTracker.h"
#include "../RiskChecker.h"

#include <cassert>
#include <cstdio>
// #include <cstring>
#include <iostream>

using namespace hft;
using namespace std;

// Helper function to Convert with Price_Scale
static int64_t fp(double p) { return static_cast<int64_t>(p * PRICE_SCALE); }

OrderEvent make_fill_request() {

    string sym, side_str, oid;
    Side side;
    int32_t quantity;
    double price;


    cout << "\nCompany Symbol: ";
    cin >> sym;

    cout << "\nBuy or Sell: ";
    cin >> side_str;

    // Convert Buy or Sell String to Side
    if (!strcmp(side_str.c_str(), "Buy")) {
        side = Side::Buy;
    } else {
        side = Side::Sell;
    }

    cout << "\nQuantity: ";
    cin >> quantity;

    cout << "\nPrice: ";
    cin >> price;

    cout << "\nOid: ";
    cin >> oid;

    cout << "\nProcessing!!!" << endl;

    return make_fill(sym, side, quantity, fp(price), oid);
}

int main() {

    cout << "Make Fill Request" << endl;
    auto apple_buy = make_fill_request();
    cout << apple_buy.symbol_view() << endl;

    return 0;
}