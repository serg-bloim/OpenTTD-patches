/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file departures.cpp Scheduled departures from a station. */

#include "stdafx.h"
#include "debug.h"
#include "gui.h"
#include "textbuf_gui.h"
#include "strings_func.h"
#include "window_func.h"
#include "vehicle_func.h"
#include "string_func.h"
#include "window_gui.h"
#include "timetable.h"
#include "vehiclelist.h"
#include "company_base.h"
#include "date_func.h"
#include "departures_gui.h"
#include "station_base.h"
#include "vehicle_gui_base.h"
#include "vehicle_base.h"
#include "vehicle_gui.h"
#include "order_base.h"
#include "settings_type.h"
#include "core/smallvec_type.hpp"
#include "date_type.h"
#include "company_type.h"
#include "cargo_type.h"
#include "departures_func.h"
#include "departures_type.h"

/** A scheduled order. */
typedef struct OrderDate
{
	const Order *order;     ///< The order
	const Vehicle *v;       ///< The vehicle carrying out the order
	DateTicks expected_date;///< The date on which the order is expected to complete
	Ticks lateness;         ///< How late this order is expected to finish
	DepartureStatus status; ///< Whether the vehicle has arrived to carry out the order yet
} OrderDate;

static bool IsDeparture(const Order *order, StationID station) {
	return (order->GetType() == OT_GOTO_STATION &&
			(StationID)order->GetDestination() == station &&
			(order->GetLoadType() != OLFB_NO_LOAD ||
			_settings_client.gui.departure_show_all_stops) &&
			order->GetWaitTime() != 0);
}

static bool IsVia(const Order *order, StationID station) {
	return ((order->GetType() == OT_GOTO_STATION ||
			order->GetType() == OT_GOTO_WAYPOINT) &&
			(StationID)order->GetDestination() == station &&
			(order->GetNonStopType() == ONSF_NO_STOP_AT_ANY_STATION ||
			order->GetNonStopType() == ONSF_NO_STOP_AT_DESTINATION_STATION));
}

static bool IsArrival(const Order *order, StationID station) {
	return (order->GetType() == OT_GOTO_STATION &&
			(StationID)order->GetDestination() == station &&
			(order->GetUnloadType() != OUFB_NO_UNLOAD ||
			_settings_client.gui.departure_show_all_stops) &&
			order->GetWaitTime() != 0);
}

/**
 * Compute an up-to-date list of departures for a station.
 * @param station the station to compute the departures of
 * @param show_vehicle_types the types of vehicles to include in the departure list
 * @param type the type of departures to get (departures or arrivals)
 * @param show_vehicles_via whether to include vehicles that have this station in their orders but do not stop at it
 * @param show_pax whether to include passenger vehicles
 * @param show_freight whether to include freight vehicles
 * @return a list of departures, which is empty if an error occurred
 */
DepartureList* MakeDepartureList(StationID station, bool show_vehicle_types[5], DepartureType type, bool show_vehicles_via, bool show_pax, bool show_freight)
{
	/* This function is the meat of the departure boards functionality. */
	/* As an overview, it works by repeatedly considering the best possible next departure to show. */
	/* By best possible we mean the one expected to arrive at the station first. */
	/* However, we do not consider departures whose scheduled time is too far in the future, even if they are expected before some delayed ones. */
	/* This code can probably be made more efficient. I haven't done so in order to keep both its (relative) simplicity and my (relative) sanity. */
	/* Having written that, it's not exactly slow at the moment. */

	/* The list of departures which will be returned as a result. */
	SmallVector<Departure*, 32> *result = new SmallVector<Departure*, 32>();

	if (!show_pax && !show_freight) return result;

	/* A list of the next scheduled orders to be considered for inclusion in the departure list. */
	SmallVector<OrderDate*, 32> next_orders;

	/* The maximum possible date for departures to be scheduled to occur. */
	DateTicks max_date = _settings_client.gui.max_departure_time * DAY_TICKS;

	/* The scheduled order in next_orders with the earliest expected_date field. */
	OrderDate *least_order = NULL;

	/* Get all the vehicles stopping at this station. */
	/* We do this to get the order which is the first time they will stop at this station. */
	/* This order is stored along with some more information. */
	/* We keep a pointer to the `least' order (the one with the soonest expected completion time). */
	for (uint i = 0; i < 4; ++i) {
		VehicleList vehicles;

		if (!show_vehicle_types[i]) {
			/* Don't show vehicles whose type we're not interested in. */
			continue;
		}

		/* MAX_COMPANIES is probably the wrong thing to put here, but it works. GenerateVehicleSortList doesn't check the company when the type of list is VL_STATION_LIST (r20801). */
		if (!GenerateVehicleSortList(&vehicles, VehicleListIdentifier(VL_STATION_LIST, (VehicleType)(VEH_TRAIN + i), MAX_COMPANIES, station))) {
			/* Something went wrong: panic! */
			return result;
		}

		/* Get the first order for each vehicle for the station we're interested in that doesn't have No Loading set. */
		/* We find the least order while we're at it. */
		for (const Vehicle **v = vehicles.Begin(); v != vehicles.End(); v++) {
			if (show_pax != show_freight) {
				bool carries_passengers = false;

				const Vehicle *u = *v;
				while (u != NULL) {
					if (u->cargo_cap > 0 && IsCargoInClass(u->cargo_type, CC_PASSENGERS)) {
						carries_passengers = true;
						break;
					}
					u = u->Next();
				}

				if (carries_passengers != show_pax) {
					continue;
				}
			}

			const Order *order = (*v)->GetOrder((*v)->cur_implicit_order_index % (*v)->GetNumOrders());
			DateTicks start_date = (DateTicks)_date_fract - (*v)->current_order_time;
			DepartureStatus status = D_TRAVELLING;

			/* If the vehicle is stopped in a depot, ignore it. */
			if ((*v)->IsStoppedInDepot()) {
				continue;
			}

			/* If the vehicle is heading for a depot to stop there, then its departures are cancelled. */
			if ((*v)->current_order.IsType(OT_GOTO_DEPOT) && (*v)->current_order.GetDepotActionType() & ODATFB_HALT) {
				status = D_CANCELLED;
			}

			if ((*v)->current_order.IsType(OT_LOADING)) {
				/* Account for the vehicle having reached the current order and being in the loading phase. */
				status = D_ARRIVED;
				start_date -= order->GetTravelTime() + (((*v)->lateness_counter < 0) ? (*v)->lateness_counter : 0);
			}

			/* Loop through the vehicle's orders until we've found a suitable order or we've determined that no such order exists. */
			/* We only need to consider each order at most once. */
			for (int i = (*v)->GetNumOrders(); i > 0; --i) {
				start_date += order->GetTravelTime() + order->GetWaitTime();

				/* If the scheduled departure date is too far in the future, stop. */
				if (start_date - (*v)->lateness_counter > max_date) {
					break;
				}

				/* If the order is a conditional branch, handle it. */
				if (order->IsType(OT_CONDITIONAL)) {
					switch(_settings_client.gui.departure_conditionals) {
							case 0: {
								/* Give up */
								break;
							}
							case 1: {
								/* Take the branch */
								if (status != D_CANCELLED) {
									status = D_TRAVELLING;
								}
								order = (*v)->GetOrder(order->GetConditionSkipToOrder());
								if (order == NULL) {
									break;
								}

								start_date -= order->GetTravelTime();

								continue;
							}
							case 2: {
								/* Do not take the branch */
								if (status != D_CANCELLED) {
									status = D_TRAVELLING;
								}
								order = (order->next == NULL) ? (*v)->GetFirstOrder() : order->next;
								continue;
							}
					}
				}

				/* Skip it if it's an automatic order. */
				if (order->IsType(OT_IMPLICIT)) {
					order = (order->next == NULL) ? (*v)->GetFirstOrder() : order->next;
					continue;
				}

				/* If an order has a 0 travel time, and it's not explictly set, then stop. */
				if (order->GetTravelTime() == 0 && !order->IsTravelTimetabled()) {
					break;
				}

				/* If the vehicle will be stopping at and loading from this station, and its wait time is not zero, then it is a departure. */
				/* If the vehicle will be stopping at and unloading at this station, and its wait time is not zero, then it is an arrival. */
				if ((type == D_DEPARTURE && IsDeparture(order, station)) ||
						(type == D_DEPARTURE && show_vehicles_via && IsVia(order, station)) ||
						(type == D_ARRIVAL && IsArrival(order, station))) {
					/* If the departure was scheduled to have already begun and has been cancelled, do not show it. */
					if (start_date < 0 && status == D_CANCELLED) {
						break;
					}

					OrderDate *od = new OrderDate();
					od->order = order;
					od->v = *v;
					/* We store the expected date for now, so that vehicles will be shown in order of expected time. */
					od->expected_date = start_date;
					od->lateness = (*v)->lateness_counter > 0 ? (*v)->lateness_counter : 0;
					od->status = status;

					/* If we are early, use the scheduled date as the expected date. We also take lateness to be zero. */
					if ((*v)->lateness_counter < 0 && !(*v)->current_order.IsType(OT_LOADING)) {
						od->expected_date -= (*v)->lateness_counter;
					}

					/* Update least_order if this is the current least order. */
					if (least_order == NULL) {
						least_order = od;
					} else if (least_order->expected_date - least_order->lateness - (type == D_ARRIVAL ? least_order->order->GetWaitTime() : 0) > od->expected_date - od->lateness - (type == D_ARRIVAL ? od->order->GetWaitTime() : 0)) {
						least_order = od;
					}

					*(next_orders.Append(1)) = od;

					/* We're done with this vehicle. */
					break;
				} else {
					/* Go to the next order in the list. */
					if (status != D_CANCELLED) {
						status = D_TRAVELLING;
					}
					order = (order->next == NULL) ? (*v)->GetFirstOrder() : order->next;
				}
			}
		}
	}

	/* No suitable orders found? Then stop. */
	if (next_orders.Length() == 0) {
		return result;
	}

	/* We now find as many departures as we can. It's a little involved so I'll try to explain each major step. */
	/* The countdown from 10000 is a safeguard just in case something nasty happens. 10000 seemed large enough. */
	for(int i = 10000; i > 0; --i) {
		/* I should probably try to convince you that this loop always terminates regardless of the safeguard. */
		/* 1. next_orders contains at least one element. */
		/* 2. The loop terminates if result->Length() exceeds a fixed (for this loop) value, or if the least order's scheduled date is later than max_date. */
		/*    (We ignore the case that the least order's scheduled date has overflown, as it is a relative rather than absolute date.) */
		/* 3. Every time we loop round, either result->Length() will have increased -OR- we will have increased the expected_date of one of the elements of next_orders. */
		/* 4. Therefore the loop must eventually terminate. */

		/* least_order is the best candidate for the next departure. */

		/* First, we check if we can stop looking for departures yet. */
		if (result->Length() >= _settings_client.gui.max_departures ||
				least_order->expected_date - least_order->lateness > max_date) {
			break;
		}

		/* We already know the least order and that it's a suitable departure, so make it into a departure. */
		Departure *d = new Departure();
		d->scheduled_date = (DateTicks)_date * DAY_TICKS + least_order->expected_date - least_order->lateness;
		d->lateness = least_order->lateness;
		d->status = least_order->status;
		d->vehicle = least_order->v;
		d->type = type;
		d->order = least_order->order;

		/* We'll be going through the order list later, so we need a separate variable for it. */
		const Order *order = least_order->order;

		if (type == D_DEPARTURE) {
			/* Computing departures: */
			/* We want to find out where it will terminate, making a list of the stations it calls at along the way. */
			/* We only count stations where unloading happens as being called at - i.e. pickup-only stations are ignored. */
			/* Where the vehicle terminates is defined as the last unique station called at by the vehicle from the current order. */

			/* If the vehicle loops round to the current order without a terminus being found, then it terminates upon reaching its current order again. */

			/* We also determine which station this departure is going via, if any. */
			/* A departure goes via a station if it is the first station for which the vehicle has an order to go via or non-stop via. */
			/* Multiple departures on the same journey may go via different stations. That a departure can go via at most one station is intentional. */

			/* We keep track of potential via stations along the way. If we call at a station immediately after going via it, then it is the via station. */
			StationID candidate_via = INVALID_STATION;

			/* Go through the order list, looping if necessary, to find a terminus. */
			/* Get the next order, which may be the vehicle's first order. */
			order = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
			/* We only need to consider each order at most once. */
			bool found_terminus = false;
			CallAt c = CallAt((StationID)order->GetDestination(), d->scheduled_date);
			for (int i = least_order->v->GetNumOrders(); i > 0; --i) {
				/* If we reach the order at which the departure occurs again, then use the departure station as the terminus. */
				if (order == least_order->order) {
					/* If we're not calling anywhere, then skip this departure. */
					found_terminus = (d->calling_at.Length() > 0);
					break;
				}

				/* If the order is a conditional branch, handle it. */
				if (order->IsType(OT_CONDITIONAL)) {
					switch(_settings_client.gui.departure_conditionals) {
							case 0: {
								/* Give up */
								break;
							}
							case 1: {
								/* Take the branch */
								order = least_order->v->GetOrder(order->GetConditionSkipToOrder());
								if (order == NULL) {
									break;
								}
								continue;
							}
							case 2: {
								/* Do not take the branch */
								order = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
								continue;
							}
					}
				}

				/* If we reach the original station again, then use it as the terminus. */
				if (order->GetType() == OT_GOTO_STATION &&
						(StationID)order->GetDestination() == station &&
						(order->GetUnloadType() != OUFB_NO_UNLOAD ||
						_settings_client.gui.departure_show_all_stops) &&
						order->GetNonStopType() != ONSF_NO_STOP_AT_ANY_STATION &&
						order->GetNonStopType() != ONSF_NO_STOP_AT_DESTINATION_STATION) {
					/* If we're not calling anywhere, then skip this departure. */
					found_terminus = (d->calling_at.Length() > 0);
					break;
				}

				/* Check if we're going via this station. */
				if ((order->GetNonStopType() == ONSF_NO_STOP_AT_ANY_STATION ||
						order->GetNonStopType() == ONSF_NO_STOP_AT_DESTINATION_STATION) &&
						order->GetType() == OT_GOTO_STATION &&
						d->via == INVALID_STATION) {
					candidate_via = (StationID)order->GetDestination();
				}

				if (c.scheduled_date != 0 && (order->GetTravelTime() != 0 || order->IsTravelTimetabled())) {
					c.scheduled_date += order->GetTravelTime();
				} else {
					c.scheduled_date = 0;
				}

				c.station = (StationID)order->GetDestination();

				/* We're not interested in this order any further if we're not calling at it. */
				if ((order->GetUnloadType() == OUFB_NO_UNLOAD &&
						!_settings_client.gui.departure_show_all_stops) ||
						(order->GetType() != OT_GOTO_STATION &&
						order->GetType() != OT_IMPLICIT) ||
						order->GetNonStopType() == ONSF_NO_STOP_AT_ANY_STATION ||
						order->GetNonStopType() == ONSF_NO_STOP_AT_DESTINATION_STATION) {
					c.scheduled_date += order->GetWaitTime();
					order = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
					continue;
				}

				/* If this order's station is already in the calling, then the previous called at station is the terminus. */
				if (d->calling_at.Contains(c)) {
					found_terminus = true;
					break;
				}

				/* If appropriate, add the station to the calling at list and make it the candidate terminus. */
				if ((order->GetType() == OT_GOTO_STATION ||
						order->GetType() == OT_IMPLICIT) &&
						order->GetNonStopType() != ONSF_NO_STOP_AT_ANY_STATION &&
						order->GetNonStopType() != ONSF_NO_STOP_AT_DESTINATION_STATION) {
					if (d->via == INVALID_STATION && candidate_via == (StationID)order->GetDestination()) {
						d->via = (StationID)order->GetDestination();
					}
					d->terminus = c;
					*(d->calling_at.Append(1)) = c;
				}

				/* If we unload all at this station, then it is the terminus. */
				if (order->GetType() == OT_GOTO_STATION &&
						order->GetUnloadType() == OUFB_UNLOAD) {
					if (d->calling_at.Length() > 0) {
						found_terminus = true;
					}
					break;
				}

				c.scheduled_date += order->GetWaitTime();

				/* Get the next order, which may be the vehicle's first order. */
				order = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
			}

			if (found_terminus) {
				/* Add the departure to the result list. */
                bool duplicate = false;

                if (_settings_client.gui.departure_merge_identical) {
                    for (uint i = 0; i < result->Length(); ++i) {
                        if (*d == **(result->Get(i))) {
                            duplicate = true;
                            break;
                        }
                    }
                }

                if (!duplicate) {
                    *(result->Append(1)) = d;

                    if (_settings_client.gui.departure_smart_terminus && type == D_DEPARTURE) {
                        for (uint i = 0; i < result->Length()-1; ++i) {
                            Departure *d_first = *(result->Get(i));
                            uint k = d_first->calling_at.Length()-2;
                            for (uint j = d->calling_at.Length(); j > 0; --j) {
                                CallAt c = CallAt(*(d->calling_at.Get(j-1)));

                                if (d_first->terminus >= c && d_first->calling_at.Length() >= 2) {
                                    d_first->terminus = CallAt(*(d_first->calling_at.Get(k)));

                                    if (k == 0) break;

                                    k--;
                                }
                            }
                        }
                    }

                    /* If the vehicle is expected to be late, we want to know what time it will arrive rather than depart. */
                    /* This is done because it looked silly to me to have a vehicle not be expected for another few days, yet it be at the same time pulling into the station. */
                    if (d->status != D_ARRIVED &&
                            d->lateness > 0) {
                        d->lateness -= least_order->order->GetWaitTime();
                    }
                }
			}
		} else {
			/* Computing arrivals: */
			/* First we need to find the origin of the order. This is somewhat like finding a terminus, but a little more involved since order lists are singly linked. */
			/* The next stage is simpler. We just need to add all the stations called at on the way to the current station. */
			/* Again, we define a station as being called at if the vehicle loads from it. */

			/* However, the very first thing we do is use the arrival time as the scheduled time instead of the departure time. */
			d->scheduled_date -= order->GetWaitTime();

			const Order *candidate_origin = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
			bool found_origin = false;

			while (candidate_origin != least_order->order) {
				if ((candidate_origin->GetLoadType() != OLFB_NO_LOAD ||
						_settings_client.gui.departure_show_all_stops) &&
						(candidate_origin->GetType() == OT_GOTO_STATION ||
						candidate_origin->GetType() == OT_IMPLICIT) &&
						candidate_origin->GetDestination() != station) {
					const Order *o = (candidate_origin->next == NULL) ? least_order->v->GetFirstOrder() : candidate_origin->next;
					bool found_collision = false;

					/* Check if the candidate origin's destination appears again before the original order or the station does. */
					while (o != least_order->order) {
						if (o->GetUnloadType() == OUFB_UNLOAD) {
							found_collision = true;
							break;
						}

						if ((o->GetType() == OT_GOTO_STATION ||
								o->GetType() == OT_IMPLICIT) &&
								(o->GetDestination() == candidate_origin->GetDestination() ||
								o->GetDestination() == station)) {
							found_collision = true;
							break;
						}

						o = (o->next == NULL) ? least_order->v->GetFirstOrder() : o->next;
					}

					/* If it doesn't, then we have found the origin. */
					if (!found_collision) {
						found_origin = true;
						break;
					}
				}

				candidate_origin = (candidate_origin->next == NULL) ? least_order->v->GetFirstOrder() : candidate_origin->next;
			}

			order = (candidate_origin->next == NULL) ? least_order->v->GetFirstOrder() : candidate_origin->next;

			while (order != least_order->order) {
				if (order->GetType() == OT_GOTO_STATION &&
						(order->GetLoadType() != OLFB_NO_LOAD ||
						_settings_client.gui.departure_show_all_stops)) {
					*(d->calling_at.Append(1)) = CallAt((StationID)order->GetDestination());
				}

				order = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
			}

			d->terminus = CallAt((StationID)candidate_origin->GetDestination());

			if (found_origin) {
                bool duplicate = false;

                if (_settings_client.gui.departure_merge_identical) {
                    for (uint i = 0; i < result->Length(); ++i) {
                        if (*d == **(result->Get(i))) {
                            duplicate = true;
                            break;
                        }
                    }
                }

                if (!duplicate) {
                    *(result->Append(1)) = d;
                }
			}
		}

		/* Save on pointer dereferences in the coming loop. */
		order = least_order->order;

		/* Now we find the next suitable order for being a departure for this vehicle. */
		/* We do this in a similar way to finding the first suitable order for the vehicle. */

		/* Go to the next order so we don't add the current order again. */
		order = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
		least_order->expected_date += order->GetTravelTime() + order->GetWaitTime();

		/* Go through the order list to find the next candidate departure. */
		/* We only need to consider each order at most once. */
		bool found_next_order = false;
		for (int i = least_order->v->GetNumOrders(); i > 0; --i) {
			/* If the order is a conditional branch, handle it. */
			if (order->IsType(OT_CONDITIONAL)) {
				switch(_settings_client.gui.departure_conditionals) {
						case 0: {
							/* Give up */
							break;
						}
						case 1: {
							/* Take the branch */
							order = least_order->v->GetOrder(order->GetConditionSkipToOrder());
							if (order == NULL) {
								break;
							}

							least_order->expected_date += order->GetWaitTime();

							continue;
						}
						case 2: {
							/* Do not take the branch */
							order = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
							least_order->expected_date += order->GetTravelTime() + order->GetWaitTime();
							continue;
						}
				}
			}

			/* Skip it if it's an automatic order. */
			if (order->IsType(OT_IMPLICIT)) {
				order = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
				continue;
			}

			/* If an order has a 0 travel time, and it's not explictly set, then stop. */
			if (order->GetTravelTime() == 0 && !order->IsTravelTimetabled()) {
				break;
			}

			/* If the departure is scheduled to be too late, then stop. */
			if (least_order->expected_date - least_order->lateness > max_date) {
				break;
			}

			/* If the order loads from this station (or unloads if we're computing arrivals) and has a wait time set, then it is suitable for being a departure. */
			if ((type == D_DEPARTURE && IsDeparture(order, station)) ||
						(type == D_DEPARTURE && show_vehicles_via && IsVia(order, station)) ||
						(type == D_ARRIVAL && IsArrival(order, station))) {
				least_order->order = order;
				found_next_order = true;
				break;
			}

			order = (order->next == NULL) ? least_order->v->GetFirstOrder() : order->next;
			least_order->expected_date += order->GetTravelTime() + order->GetWaitTime();
		}

		/* If we didn't find a suitable order for being a departure, then we can ignore this vehicle from now on. */
		if (!found_next_order) {
			/* Make sure we don't try to get departures out of this order. */
			/* This is cheaper than deleting it from next_orders. */
			/* If we ever get to a state where _date * DAY_TICKS is close to INT_MAX, then we'll have other problems anyway as departures' scheduled dates will wrap around. */
			least_order->expected_date = INT32_MAX;
		}

		/* The vehicle can't possibly have arrived at its next candidate departure yet. */
		if (least_order->status == D_ARRIVED) {
			least_order->status = D_TRAVELLING;
		}

		/* Find the new least order. */
		for (uint i = 0; i < next_orders.Length(); ++i) {
			OrderDate *od = *(next_orders.Get(i));

			DateTicks lod = least_order->expected_date - least_order->lateness;
			DateTicks odd = od->expected_date - od->lateness;

			if (type == D_ARRIVAL) {
				lod -= least_order->order->GetWaitTime();
				odd -= od->order->GetWaitTime();
			}

			if (lod > odd && od->expected_date - od->lateness < max_date) {
				least_order = od;
			}
		}
	}

	/* Done. Phew! */
	return result;
}
