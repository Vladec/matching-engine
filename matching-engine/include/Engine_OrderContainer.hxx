/*
* Copyright (C) 2015, Fabien Aulaire
* All rights reserved.
*/

#include <Engine_OrderContainer.h>
#include <Engine_Deal.h>
#include <Engine_Tools.h>

#include <iomanip>

namespace exchange
{
    namespace engine
    {

        template <typename TOrder, typename TEventHandler>
        void OrderContainer<TOrder, TEventHandler>::CancelAllOrders()
        {
            auto DoCancelAllOrders = [this](auto & container)
            {
                while (!container.empty())
                {
                    auto OrderIt = container.begin();
                    m_EventHandler.OnUnsolicitedCancelledOrder(*OrderIt);
                    container.erase(OrderIt);
                }
            };

            DoCancelAllOrders(m_AskOrders);
            DoCancelAllOrders(m_BidOrders);
        }

        template <typename TOrder, typename SortingPredicate>
        struct ExecutableQtyPredHelper
        {
            typedef std::greater_equal<typename TOrder::price_type>    value;
        };

        template <typename TOrder>
        struct ExecutableQtyPredHelper<TOrder, std::less<typename TOrder::price_type> >
        {
            typedef std::less_equal<typename TOrder::price_type>       value;
        };

        template <typename TOrder, typename TEventHandler>
        template <typename Container>
        Quantity OrderContainer<TOrder, TEventHandler>::GetExecutableQuantity(const Container & Orders, price_type iPrice) const
        {
            auto & Index = bmi::get<price_tag>(Orders);
            auto Qty = 0_qty;

            typedef decltype(Index.key_comp())                                          SortingPredicate;
            typedef typename ExecutableQtyPredHelper<TOrder, SortingPredicate>::value   Predicate;

            for (auto & order : Index)
            {
                if (Predicate()(order.GetPrice(), iPrice))
                {
                    Qty += order.GetQuantity();
                }
            }
            return Qty;
        }

        template <typename TOrder, typename TEventHandler>
        template <typename Msg>
        Quantity OrderContainer<TOrder, TEventHandler>::GetExecutableQuantity(const Msg & iMsg, OrderWay iWay) const
        {
            switch (iWay)
            {
                case OrderWay::BUY:
                {
                    auto MaxQty = GetExecutableQuantity(m_AskOrders, iMsg.GetPrice());
                    return (std::min)(MaxQty, iMsg.GetQuantity());
                }
                case OrderWay::SELL:
                {
                    auto MaxQty = GetExecutableQuantity(m_BidOrders, iMsg.GetPrice());
                    return (std::min)(MaxQty, iMsg.GetQuantity());
                }
                default:
                    return 0_qty;
            }
        }

        template <typename Msg, bool bIsReplaceOrder>
        struct AggressorIDHelper
        {
            static std::uint32_t get(Msg & t) { return t.GetOrderID(); }
        };

        template <typename Msg>
        struct AggressorIDHelper<Msg, true>
        {
            static std::uint32_t get(Msg & t) { return t.GetReplacedOrderID(); }
        };

        template <typename Msg>
        std::uint32_t GetAggressorID(Msg & t)
        {
            return AggressorIDHelper<Msg, is_replaced_order<Msg>::value >::get(t);
        }

        template <typename TOrder, typename TEventHandler>
        template <typename Container, typename Msg>
        void OrderContainer<TOrder, TEventHandler>::ProcessDeals(Container & Orders, Msg & iMsg, Quantity iMatchQty)
        {
            auto & Index = bmi::get<price_tag>(Orders);

            while (iMatchQty > 0_qty)
            {
                auto OrderToHit = Index.begin();

                // Get the exec price and the exec quantity
                auto ExecQty = (std::min)(OrderToHit->GetQuantity(), iMsg.GetQuantity());
                auto ExecPrice = (std::min)(OrderToHit->GetPrice(), iMsg.GetPrice());

                // Update quantity of both orders
                iMsg.RemoveQuantity(ExecQty);
                Index.modify(OrderToHit, OrderUpdaterSingle<&Order::SetQuantity>(OrderToHit->GetQuantity() - ExecQty));

                // Decrease the matching quantity
                iMatchQty -= ExecQty;

                // Generate the deal
                std::unique_ptr<Deal> pDeal = nullptr;

                if (OrderToHit->GetWay() == OrderWay::BUY)
                {
                    pDeal = std::make_unique<Deal>(ExecPrice, ExecQty, OrderToHit->GetClientID(), OrderToHit->GetOrderID(), iMsg.GetClientID(), GetAggressorID(iMsg));
                }
                else
                {
                    pDeal = std::make_unique<Deal>(ExecPrice, ExecQty, iMsg.GetClientID(), GetAggressorID(iMsg), OrderToHit->GetClientID(), OrderToHit->GetOrderID());
                }

                m_EventHandler.OnDeal(std::move(pDeal));

                if (0_qty == OrderToHit->GetQuantity())
                {
                    Index.erase(OrderToHit);
                }
            }
        }

        template <typename TOrder, typename TEventHandler>
        template <typename Msg>
        void OrderContainer<TOrder, TEventHandler>::ProcessDeals(Msg & iMsg, OrderWay iWay, Quantity iMatchQty)
        {
            switch (iWay)
            {
                case OrderWay::BUY:
                    ProcessDeals(m_AskOrders, iMsg, iMatchQty);
                    break;
                case OrderWay::SELL:
                    ProcessDeals(m_BidOrders, iMsg, iMatchQty);
                    break;
                default:
                    assert(false);
                    return;
            }
        }

        template <typename TOrder, typename TEventHandler>
        bool OrderContainer<TOrder, TEventHandler>::Insert(TOrder & iOrder, bool Match)
        {
            auto OrderID = OrderIDGenerator<TOrder>()(iOrder);
            if (m_InsertedOrderIDs.find(OrderID) != m_InsertedOrderIDs.end())
            {
                return false;
            }

            if (Match)
            {
                auto MatchQty = (std::min)(GetExecutableQuantity(iOrder, iOrder.GetWay()), iOrder.GetQuantity());

                if (MatchQty != 0_qty)
                {
                    ProcessDeals(iOrder, iOrder.GetWay(), MatchQty);
                }
            }

            if (iOrder.GetQuantity() != 0_qty)
            {
                if (AuctionInsert(iOrder) == false )
                {
                    return false;
                }
            }

            m_InsertedOrderIDs.insert(OrderID);

            return true;
        }

        template <typename TOrder, typename TEventHandler>
        bool OrderContainer<TOrder, TEventHandler>::AuctionInsert(const TOrder & iOrder)
        {
            /*
            *  insert return a pair, the second element indicate
            *  if the insertion fail or not.
            */
            switch (iOrder.GetWay())
            {
                case OrderWay::BUY:
                    return m_BidOrders.insert(iOrder).second;
                case OrderWay::SELL:
                    return m_AskOrders.insert(iOrder).second;
                default:
                    assert(false);
                    return false;
            };
        }

        /**
        * Erase an order from order book
        */
        template <typename TOrder, typename TEventHandler>
        bool OrderContainer<TOrder, TEventHandler>::Delete(const std::uint32_t iOrderId, const std::uint32_t iClientId, OrderWay iWay)
        {    
            switch (iWay)
            {
                case OrderWay::BUY:
                    return (1 == m_BidOrders.erase(OrderIDGenerator<TOrder>()(iClientId, iOrderId)));
                case OrderWay::SELL:
                    return (1 == m_AskOrders.erase(OrderIDGenerator<TOrder>()(iClientId, iOrderId)));
                default:
                    assert(false);
                    return false;
            };
        }

        template <typename TOrder, typename TEventHandler>
        template <typename TOrderReplace>
        bool OrderContainer<TOrder, TEventHandler>::Modify(TOrderReplace & iOrderReplace, bool Match)
        {
            auto ProcessModify = [&]()
            {
                if (Match)
                {
                    auto MaxExecQty = GetExecutableQuantity(iOrderReplace, iOrderReplace.GetWay());
                    auto MatchQty = (std::min)(MaxExecQty, iOrderReplace.GetQuantity());

                    if (MatchQty != 0_qty)
                    {
                        ProcessDeals(iOrderReplace, iOrderReplace.GetWay(), MatchQty);

                        if (0_qty == iOrderReplace.GetQuantity())
                        {
                            return false;
                        }
                    }
                }
                return true;
            };

            auto OrderID    = OrderIDGenerator<TOrder>()(iOrderReplace.GetClientID(), iOrderReplace.GetExistingOrderID());
            auto NewOrderID = OrderIDGenerator<TOrder>()(iOrderReplace.GetClientID(), iOrderReplace.GetReplacedOrderID());

            if (m_InsertedOrderIDs.find(NewOrderID) != m_InsertedOrderIDs.end())
            {
                return false;
            }

            auto ApplyModify = [&](auto & Container)
            {
                auto Order = Container.find(OrderID);
                if (Order != Container.end())
                {
                    if (ProcessModify())
                    {
                        // Order is not fully filled, re-queued the rest of the quantity
                        
                        TOrder ReplacedOrder = { iOrderReplace.GetWay(), iOrderReplace.GetQuantity(), iOrderReplace.GetPrice(),
                                         iOrderReplace.GetReplacedOrderID(), iOrderReplace.GetClientID() };

                        Container.erase(Order);
                        Container.insert(ReplacedOrder);
                    }
                    else
                    {
                        // No quantity left on the order, erase it from the container
                        Container.erase(Order);
                    }
                    m_InsertedOrderIDs.insert(OrderID);
                    return true;
                }
                return false;
            };

            switch (iOrderReplace.GetWay())
            {
                case OrderWay::BUY:
                    return ApplyModify(m_BidOrders);
                case OrderWay::SELL:
                    return ApplyModify(m_AskOrders);
                default:
                    assert(false);
                    return false;
            }
        }

        /*
            ENH_TODO : Check if the following rule is applied
            Where there are two maximum executable volumes (for two different prices), the following rules are applied:
            the opening price cannot be higher than the best ask or lower than the best bid immediately after the auction phase.
            This rules is clearly not applied, but because the matching engine does not support market order
            for now, this rules is not necessary
        */
        template <typename TOrder, typename TEventHandler>
        auto OrderContainer<TOrder, TEventHandler>::GetTheoriticalAuctionInformations() const -> OpenInformationType
        {
            auto MaxQty = 0_qty;
//            std::uint64_t MaxQty = 0;
            auto OpenPrice = 0_price;

            for (auto & order : m_AskOrders)
            {
//                std::uint64_t BidQty = GetExecutableQuantity(m_BidOrders, order.GetPrice());
//                std::uint64_t AskQty = GetExecutableQuantity(m_AskOrders, order.GetPrice());
                auto BidQty = GetExecutableQuantity(m_BidOrders, order.GetPrice());
                auto AskQty = GetExecutableQuantity(m_AskOrders, order.GetPrice());


                auto CurrentQty = (std::min)(BidQty, AskQty);

                if (CurrentQty > MaxQty)
                {
                    MaxQty = CurrentQty;
                    OpenPrice = order.GetPrice();
                }
            }
            return std::make_tuple(OpenPrice, MaxQty);
        }


        /*
        Post auction phase
        */
        template <typename TOrder, typename TEventHandler>
        void  OrderContainer<TOrder, TEventHandler>::MatchOrders()
        {
            auto OpeningInformation = GetTheoriticalAuctionInformations();

            auto MatchingPrice( std::get<0>(OpeningInformation) );
            auto MatchingQty  ( std::get<1>(OpeningInformation) );

            bid_index_type & BidIndex = GetBidIndex();
            ask_index_type & AskIndex = GetAskIndex();

            while (MatchingQty > 0_qty)
            {
                price_index_iterator BidOrder = BidIndex.begin();

                while (BidOrder->GetQuantity() > 0_qty && MatchingQty > 0_qty)
                {
                    price_index_iterator AskOrder = AskIndex.begin();

                    auto ExecutedQty = (std::min)(AskOrder->GetQuantity(), BidOrder->GetQuantity());

                    AskIndex.modify(AskOrder, OrderUpdaterSingle<&Order::SetQuantity>(AskOrder->GetQuantity() - ExecutedQty));
                    BidIndex.modify(BidOrder, OrderUpdaterSingle<&Order::SetQuantity>(BidOrder->GetQuantity() - ExecutedQty));

                    std::unique_ptr<Deal> pDeal = std::make_unique<Deal>(MatchingPrice, ExecutedQty, BidOrder->GetClientID(), BidOrder->GetOrderID(), AskOrder->GetClientID(), AskOrder->GetOrderID());
                    m_EventHandler.OnDeal(std::move(pDeal));

                    MatchingQty -= ExecutedQty;

                    if (0_qty == AskOrder->GetQuantity())
                    {
                        AskIndex.erase(AskOrder);
                    }
                }

                if (0_qty == BidOrder->GetQuantity())
                {
                    GetBidIndex().erase(BidOrder);
                }
            }
        }

        template <typename TOrder, typename TEventHandler>
        void OrderContainer<TOrder, TEventHandler>::ByOrderView(std::vector<TOrder> & BidContainer, std::vector<TOrder> & AskContainer) const
        {
            BidContainer.reserve( GetBidIndex().size() );
            AskContainer.reserve( GetAskIndex().size() );

            std::copy(GetAskIndex().begin(), GetAskIndex().end(), std::back_inserter(AskContainer));
            std::copy(GetBidIndex().begin(), GetBidIndex().end(), std::back_inserter(BidContainer));
        }

        template <typename TOrder, typename TEventHandler>
        void OrderContainer<TOrder, TEventHandler>::AggregatedView(LimitContainer & BidContainer, LimitContainer & AskContainer) const
        {
            auto AggregatedViewHelper = [&](price_index_iterator begin, price_index_iterator end, LimitContainer & Container)
            { 
                price_type CurrentPrice = std::numeric_limits<price_type>::max();
                size_t     ContainerIndex = 0;

                for (; begin != end; begin++)
                {
                    price_type Price = begin->GetPrice();
                    if (Price != CurrentPrice)
                    {
                        ContainerIndex++;
                        CurrentPrice = Price;
                        Container.emplace_back(0, 0_qty, Price);
                    }

                    LimitType & Limit = Container[ContainerIndex - 1];
                    std::get<0>(Limit)++;
                    std::get<1>(Limit) += begin->GetQuantity();
                }
            };

            AggregatedViewHelper(GetBidIndex().begin(), GetBidIndex().end(), BidContainer);
            AggregatedViewHelper(GetAskIndex().begin(), GetAskIndex().end(), AskContainer);
        }

        template <typename TOrder, typename TEventHandler>
        std::ostream& operator<<(std::ostream& oss, const OrderContainer<TOrder, TEventHandler> & iOrders)
        {
            switch (iOrders.GetViewMode())
            {
                case OrderContainer<TOrder, TEventHandler>::ViewMode::VM_BY_ORDER:
                    iOrders.StreamByOrder(oss);
                    break;
                case OrderContainer<TOrder, TEventHandler>::ViewMode::VM_BY_PRICE:
                    iOrders.StreamByPrice(oss);
                    break;
                case OrderContainer<TOrder, TEventHandler>::ViewMode::VM_UNKNOWN:
                    assert(false);
                    break;
            };
            return oss;
        }

        template <typename TOrder, typename TEventHandler>
        void OrderContainer<TOrder, TEventHandler>::StreamByOrder(std::ostream& oss) const
        {
            auto MaxIndex = (std::max)(m_BidOrders.size(), m_AskOrders.size());

            auto MakeString = [](Quantity qty, Price price)
            {
                std::ostringstream oss("");
                oss << qty.AsScalar() << "@" << price.AsScalar();
                return oss.str();
            };

            auto StreamEntry = [&](price_index_iterator & Entry, price_index_iterator end, const std::string & end_string)
            {
                if (Entry != end)
                {
                    oss << "|" << std::setw(13) << MakeString(Entry->GetQuantity(), Entry->GetPrice()) << end_string;
                    Entry++;
                }
                else
                {
                    oss << "|" << std::setw(13) << "0" << end_string;
                }
            };

            oss << "|        BID         |        ASK        |" << std::endl;
            oss << "|                    |                   |" << std::endl;

            auto AskIterator = GetAskIndex().begin();
            auto BidIterator = GetBidIndex().begin();

            for (std::uint64_t Index = 0; Index < MaxIndex; Index++)
            {
                StreamEntry(BidIterator, GetBidIndex().end(), "       ");
                StreamEntry(AskIterator, GetAskIndex().end(), "      |");
                oss << std::endl;
            }
        }

        template <typename TOrder, typename TEventHandler>
        void OrderContainer<TOrder, TEventHandler>::StreamByPrice(std::ostream& oss) const
        {
            typedef typename OrderContainer<TOrder, TEventHandler>::LimitContainer LimitContainer;
            typedef typename OrderContainer<TOrder, TEventHandler>::LimitType      LimitType;

            LimitContainer BidContainer;
            LimitContainer AskContainer;

            auto MakeString = [](std::uint32_t NbOrder, Quantity qty, Price price)
            {
                std::ostringstream oss("");
                oss << "  " << NbOrder << "   " << qty.AsScalar() << "@" << price.AsScalar();
                return oss.str();
            };

            auto StreamEntry = [&](size_t Index, LimitContainer & Container, const std::string & end_string)
            {
                if (Index < Container.size())
                {
                    LimitType & Limit = Container[Index];
                    oss << "|" << std::setw(15) << MakeString(std::get<0>(Limit), std::get<1>(Limit), std::get<2>(Limit)) << end_string;
                }
                else
                {
                    oss << "|" << std::setw(15) << "0" << end_string;
                }
            };
            
            AggregatedView(BidContainer, AskContainer);

            auto MaxIndex = (std::max)(BidContainer.size(), AskContainer.size());

            oss << "|         BID          |         ASK         |" << std::endl;
            oss << "|                      |                     |" << std::endl;

            for (std::uint64_t Index = 0; Index < MaxIndex; Index++)
            {
                StreamEntry(Index, BidContainer, "       ");
                StreamEntry(Index, AskContainer, "      |");
                oss << std::endl;
            }
        }

    }
}
