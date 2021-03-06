/*
* Copyright (C) 2016, Fabien Aulaire
* All rights reserved.
*/

#include <ostream>

#include <Engine_Deal.h>

namespace exchange
{
    namespace engine
    {

        Deal::Deal(price_type iPrice, qty_type iQty, client_id_type iBuyerClientID, client_orderid_type iBuyerOrderID, client_id_type iSellerClientID, client_orderid_type iSellerOrderID)
            : m_Reference{0}, m_Price(iPrice), m_Qty(iQty), m_BuyerClientID(iBuyerClientID),
            m_BuyerOrderID(iBuyerOrderID), m_SellerClientID(iSellerClientID), m_SellerOrderID(iSellerOrderID)
        {
            m_Timestamp = std::chrono::system_clock::now();
        }

        bool Deal::operator == (const Deal & rhs) const
        {
            if(&rhs != this)
            {
                return ( m_Price == rhs.GetPrice() ) && ( m_Qty == rhs.GetQuantity() ) &&
                       (m_BuyerClientID == rhs.GetBuyerClientID()) && (m_BuyerOrderID == rhs.GetBuyerOrderID()) &&
                       (m_SellerClientID == rhs.GetSellerClientID()) && (m_SellerOrderID == rhs.GetSellerOrderID());
            }
            return true;
        }

        std::ostream& operator<<(std::ostream& o, const Deal & x)
        {
            o << "Deal : Price[" << x.GetPrice() << "] ; Qty[" << x.GetQuantity() << "] ;"
                << " BuyerClientID[" << x.GetBuyerClientID() << "] ; BuyerOrderID[" << x.GetBuyerOrderID()
                << "] ; SellerClientID[" << x.GetSellerClientID() << "] ; SellerOrderID[" << x.GetSellerOrderID() << "] ;"
                << " Reference[" << x.GetReference().data() << "]";
            return o;
        }

    }
}
