// Copyright (c) 2018 the Cargo authors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include <omp.h>
#include <algorithm> /* std::find, std::nth_element */
#include <chrono>
#include <ctime>
#include <exception>
#include <iostream> /* std::endl */
#include <vector>

#include "libcargo.h"
#include "trip_vehicle_grouping.h"

#include "glpk/glpk.h"

using namespace cargo;

const int BATCH    = 30;
const int RANGE    = 2000; // meters
const int TOP_CUST = 30;  // customers per vehicle for rv-graph
const int TRIP_MAX = 15000;  // maximum number of trips per batch

TripVehicleGrouping::TripVehicleGrouping()
    : RSAlgorithm("trip_vehicle_grouping", false), grid_(100) {
  batch_time() = BATCH;
  if (!omp_get_cancellation()) {
    print(MessageType::Error) << "OMP_CANCELLATION not set"
        << " (export OMP_CANCELLATION=true)" << std::endl;
    throw;
  }
  for (int i = 0; i < omp_get_max_threads(); ++i)
    gtre_.push_back(GTree::get());
}

void TripVehicleGrouping::match() {
  this->beg_ht();
  this->reset_workspace();
  this->timeout_rv_0 = hiclock::now();
  for (const Customer& cust : customers())
    is_matched[cust.id()] = false;

  /* Generate rv-graph */
  { std::vector<Customer> lcl_cust = customers();
  // print << "generating rv-graph..." << std::endl;
  #pragma omp parallel shared(lcl_cust, rvgrph_rr_, rvgrph_rv_, \
         rv_cst, rv_sch, rv_rte, matchable_custs)
  { /* Each thread gets a local gtree and a local grid to perform
     * sp-computations in parallel */
  GTree::G_Tree& lcl_gtre = gtre_.at(omp_get_thread_num());
  Grid lcl_grid = grid_;
  #pragma omp for
  for (auto ptcust = lcl_cust.begin(); ptcust < lcl_cust.end(); ++ptcust) {
    #pragma omp critical
    { matchable_custs.push_back(ptcust->id()); }
    const Customer& cust_a = *ptcust;
    // print << "Working on cust " << ptcust->id() << std::endl;

    /* Build rv edges
     * ----------------- */
    // print << "building rv-edges..." << std::endl;
    auto cands = lcl_grid.within(RANGE, cust_a.orig());
    for (const auto& cand : cands) {
      // print << "  checking vehl " << cand->id() << std::endl;
      if (cand->queued() < cand->capacity()) {
        DistInt cstout = 0;
        std::vector<Stop> schout;
        std::vector<Wayp> rteout;
        if (travel(*cand, {cust_a}, cstout, schout, rteout, lcl_gtre)) {
          // print << "  accept" << std::endl;
          #pragma omp critical
          { rv_cst[*cand][cust_a] = cstout;
            rv_sch[*cand][cust_a] = schout;
            rv_rte[*cand][cust_a] = rteout; }
        } else {
            // print << "  travel not valid; reject" << std::endl;
        }
      } else {
        // print << "  no capacity; reject" << std::endl;
      }
    }
     if (cands.size() == 0) {
       continue;
       print << "No candidates." << std::endl;
     }
    if (this->timeout(this->timeout_rv_0)) {
      // print << "Timed out" << std::endl;
      // break;
      #pragma omp cancel for
    }

    /* Build rr edges
     * ----------------- */
    // print << "building rr-edges..." << std::endl;
    Vehicle vtvehl(cust_a.id(), cust_a.orig(), cust_a.dest(),
                   cust_a.early(), cust_a.late(), 0, lcl_gtre);
    /* ...then compare against all other cust_b */
    for (const Customer& cust_b : customers()) {
      if (cust_a == cust_b) continue;
      // print << "Cust " << cust_a.id() << " join Cust " << cust_b.id() << " ? "
      //      << std::endl;
      /* Euclidean filters */
      if (haversine(cust_a.orig(), cust_b.orig()) > RANGE) {
        // print << "  origins not in range; reject" << std::endl;
        continue;
      }
      if (haversine(cust_a.dest(), cust_b.dest()) > RANGE) {
        // print << "  destinations not in range; reject" << std::endl;
        continue;
      }
      DistInt cstout = 0;
      std::vector<Stop> schout;
      std::vector<Wayp> rteout;
      if (travel(vtvehl, {cust_b}, cstout, schout, rteout, lcl_gtre)) {
        // print << "  accept" << std::endl;
        #pragma omp critical
        { rvgrph_rr_[cust_a].push_back(cust_b); }
      } else {
        // print << "  travel not valid; reject" << std::endl;
      }
    }
    // print << "Total rr-edges for cust " << ptcust->id() << std::endl;
    // for (const auto& i : rvgrph_rr_) {
    //   print << "Cust " << i.first.id() << ": Custs ";
    //   for (const auto& j : i.second)
    //     print << j.id() << " ";
    //   print << std::endl;
    // }
    // print << "done" << std::endl;
  }  // end pragma omp for
  }  // end pragma omp parallel
  }  // end generate rv-graph

  { /* Heuristic: keep only the lowest k customers per vehicle */
    size_t topk = TOP_CUST;
    for (const auto& kv : rv_cst) {
      const Vehicle& vehl = kv.first;
      if (kv.second.size() > topk) {
        std::vector<std::pair<Customer, DistInt>> cc;
        for (const auto& kv2 : kv.second) cc.push_back({kv2.first, kv2.second});
        std::nth_element(cc.begin(), cc.begin() + topk, cc.end(),
                         [](const std::pair<Customer, DistInt>& a,
                            const std::pair<Customer, DistInt>& b) {
                           return a.second < b.second; });
        for (size_t i = 0; i < topk && i < kv.second.size(); ++i)
          rvgrph_rv_[vehl].push_back(cc.at(i).first);
      } else {
        for (const auto& kv2 : kv.second)
          rvgrph_rv_[vehl].push_back(kv2.first);
      }
    }
  }
  // print << "Total rv-edges:" << std::endl;
  // for (const auto& i : rvgrph_rr_) {
  //   print << "Vehl " << i.first.id() << ": Custs ";
  //   for (const auto& j : i.second)
  //     print << j.id() << " ";
  //   print << std::endl;
  // }
  // print << "done" << std::endl;

  if (this->done())
    return;

  /* Generate rtv-graph */
  this->timeout_rtv_0 = hiclock::now();
  int nvted = 0;
  { std::vector<Vehicle> lcl_vehl = vehicles();
  #pragma omp parallel shared(nvted, lcl_vehl, vt_sch, vt_rte, vehmap, \
          vted_, rvgrph_rr_, rvgrph_rv_)
  { /* Each thread adds edges to a local vtedges; these are combined when
     * all threads have completed */
  dict<VehlId, dict<SharedTripId, DistInt>> lcl_vted = {};
  dict<SharedTripId, SharedTrip>            lcl_trip = {};
  GTree::G_Tree                           & lcl_gtre = gtre_.at(omp_get_thread_num());
  #pragma omp for
  for (auto ptvehl = lcl_vehl.begin(); ptvehl < lcl_vehl.end(); ++ptvehl) {
    const Vehicle& vehl = *ptvehl;
    if (vehl.queued() == vehl.capacity())
      continue; // skip assigned to full
    #pragma omp critical // store vehicle for later
    { vehmap[vehl.id()] = vehl; }

    /* Store trips of size_t for this vehicle */
    dict<size_t, std::vector<SharedTripId>> tripk;

    /* Trips of size 1
     * ---------------
     *  Add a trip of size 1 for every rv-pair involving this vehl */
    // print << "Building trips size 1 for Vehl " << vehl.id() << "..." << std::endl;
    if (rvgrph_rv_.count(vehl) > 0) {
      for (const Customer& cust : rvgrph_rv_.at(vehl)) {
        SharedTripId stid;
        #pragma omp critical
        { stid = add_trip({cust});
          vt_sch[vehl.id()][stid] = rv_sch[vehl][cust];
          vt_rte[vehl.id()][stid] = rv_rte[vehl][cust]; }
        lcl_vted[vehl.id()][stid] = rv_cst[vehl][cust];
        lcl_trip[stid] = {cust};
        tripk[1].push_back(stid);
      }
    } else
      continue;  // <-- if no rv-pairs, skip to the next vehl
    if (this->timeout(this->timeout_rtv_0)) {
      // print << "Timed out" << std::endl;
      // break;
      #pragma omp cancel for
    }

    /* Trips of size 2
     * ---------------
     * Add trips of size 1 that can be combined, and add any rr-pairs
     * that can be served by this vehl */
    if (vehl.capacity() - vehl.queued() > 1) {  // <-- only vehls with capac
    // print << "Building trips size 2 for Vehl " << vehl.id() << "..." << std::endl;
    /* 1) Check if any size 1 trips can be combined */
    for (const auto& id_a : tripk.at(1)) {
      SharedTrip shtrip(lcl_trip.at(id_a));
      for (const auto& id_b : tripk.at(1)) {
        if (id_a == id_b) continue;
        shtrip.insert(shtrip.end(), lcl_trip.at(id_b).begin(), lcl_trip.at(id_b).end());
        /* Euclidean filter */
        if (haversine(vehl.last_visited_node(), shtrip.at(0).orig()) > RANGE)
          continue;
        DistInt cstout = 0;
        std::vector<Stop> schout;
        std::vector<Wayp> rteout;
        if (travel(vehl, shtrip, cstout, schout, rteout, lcl_gtre)) {
          SharedTripId stid;
          #pragma omp critical
          { stid = add_trip(shtrip);
            vt_sch[vehl.id()][stid] = schout;
            vt_rte[vehl.id()][stid] = rteout; }
          lcl_vted[vehl.id()][stid] = cstout;
          lcl_trip[stid] = shtrip;
          tripk[2].push_back(stid);
        }
      }
    }
    if (this->timeout(this->timeout_rtv_0)) {
      // print << "Timed out" << std::endl;
      // break;
      #pragma omp cancel for
    }

    /* 2) Check if any rr pairs can be served by this vehicle */
    for (const auto& kv : rvgrph_rr_) {
      const Customer& cust_a = kv.first;
      /* Euclidean filter */
      if (haversine(vehl.last_visited_node(), cust_a.orig()) > RANGE) continue;
      for (const Customer& cust_b : kv.second) {
        DistInt cstout = 0;
        std::vector<Stop> schout;
        std::vector<Wayp> rteout;
        SharedTrip shtrip = {cust_a, cust_b};
        if (travel(vehl, shtrip, cstout, schout, rteout, lcl_gtre)) {
          SharedTripId stid;
          #pragma omp critical
          { stid = add_trip(shtrip);
            vt_sch[vehl.id()][stid] = schout;
            vt_rte[vehl.id()][stid] = rteout; }
          lcl_vted[vehl.id()][stid] = cstout;
          lcl_trip[stid] = shtrip;
          tripk[2].push_back(stid);
        }
      }
    }
    if (this->timeout(this->timeout_rtv_0)) {
      // print << "Timed out" << std::endl;
      // break;
      #pragma omp cancel for
    }

    /* Trips of size >= 3
     * ------------------ */
    size_t k = 3;
    while ((size_t)(vehl.capacity() - vehl.queued()) >= k && tripk.count(k-1) > 0) {
      // print << "Building trips size " << k << " for Vehl " << vehl.id() << "..." << std::endl;
      /* Loop through trips of size k-1 */
      for (SharedTripId id_a : tripk.at(k - 1)) {
        const SharedTrip& trip_a = lcl_trip.at(id_a);
        /* Compare against remaining trips of size k-1 */
        for (SharedTripId id_b : tripk.at(k - 1)) {
          if (id_a == id_b) continue;
          const SharedTrip& trip_b = lcl_trip.at(id_b);
          /* Join trip_a and trip_b into new trip (no dups) */
          SharedTrip shtrip(trip_a);
          for (const Customer& cust : trip_b)
            if (std::find(shtrip.begin(), shtrip.end(), cust) == shtrip.end())
              shtrip.push_back(cust);
          /* If shtrip is size k... */
          if (shtrip.size() == k) {
            bool all_ok = true;
            /* ... check each subtrip if it is already a trip */
            for (size_t p = 0; p < shtrip.size(); ++p) {
              bool subtrip_ok = false;
              SharedTrip subtrip(shtrip);
              subtrip.erase(subtrip.begin() + p);
              for (SharedTripId id_q : tripk.at(k - 1))
                if (subtrip == lcl_trip.at(id_q)) {
                  subtrip_ok = true;
                  break;
                }
              if (!subtrip_ok) {
                all_ok = false;
                break;
              }
            }
            if (all_ok) {
              DistInt cstout = 0;
              std::vector<Stop> schout;
              std::vector<Wayp> rteout;
              if (travel(vehl, shtrip, cstout, schout, rteout, lcl_gtre)) {
                SharedTripId stid;
                #pragma omp critical
                { stid = add_trip(shtrip);
                  vt_sch[vehl.id()][stid] = schout;
                  vt_rte[vehl.id()][stid] = rteout; }
                lcl_vted[vehl.id()][stid] = cstout;
                lcl_trip[stid] = shtrip;
                tripk[k].push_back(stid);
              }
            }
          }  // end if shtrip.size() == k
        }  // end inner for
        // if (timed_out) break;
      } // end outer for
      k++;
    } // end while
    } // end if vehls with capacity
    if (this->timeout(timeout_rtv_0)) {
      // print << "Timed out" << std::endl;
      // break;
      #pragma omp cancel for
    }
  } // end pragma omp for

  /* Combine lcl_vted */
  #pragma omp critical
  {
    for (const auto& kv : lcl_vted) {
    if (vted_.count(kv.first) == 0) {
      vted_[kv.first] = kv.second;
      nvted += kv.second.size();
      /* Heuristic: keep up to TRIP_MAX trips */
      if (nvted > TRIP_MAX) break;
    } else {
      for (const auto& kv2 : kv.second) {
        vted_[kv.first][kv2.first] = kv2.second;
        nvted++;
        if (nvted > TRIP_MAX) break;
       }
    }
    if (nvted > TRIP_MAX) break;
    }  // end for
  }    // end pragma omp critical

  }  // end pragma omp parallel
  }  // end generate rtv-graph

  if (this->done())
    return;

  /* Generating the mip */
  if (vted_.size() > 0) {
  /* Objective: c11x11 + c12x12 + ... + cijxij + y1 + y2 + ... + yn
   *     For cijxij, ij are from vted_
   *     For yn, n is from waiting_customers()
   * Constraints:
   *     1) Each vehicle serves 0 or 1 trips
   *         xi1 + xi2 + ... + xin <= 1, for all i
   *     2) Each customer is either served or unserved
   *         yn + xij == 1, for all n, where j are trips containing n, and
   *                                         i are the vehicles serve them
   * Properties:
   *     - Total terms in the objective == number of columns
   *       == all VehicleId, SharedTripId pairs in vted_, plus |customers| */
  glp_prob* mip;
  mip = glp_create_prob();
  glp_set_prob_name(mip, ("mip (t=" + std::to_string(Cargo::now())).c_str());
  glp_set_obj_dir(mip, GLP_MIN);
  glp_term_out(GLP_OFF);

  int ncol = nvted + matchable_custs.size();
  glp_add_cols(mip, ncol);
  /*     - Total rows == # of vehicles (size of vted_), plus |customers| */
  int nrow = vted_.size() + matchable_custs.size();
  glp_add_rows(mip, nrow);

  /* Map col_idx to the vehicle/trip edge */
  dict<size_t, std::pair<TripId, SharedTripId>> colmap{};

  { /* Populate the coefficients in the objective */
  size_t col_idx = 0;
  for (const auto& kv : vted_) {
    const VehlId& vehl_id = kv.first;
    for (const auto& kv2 : kv.second) {
      const SharedTripId& shtrip_id = kv2.first;
      col_idx++;
      colmap[col_idx] = {vehl_id, shtrip_id};
      glp_set_obj_coef(mip, col_idx, kv2.second);
      glp_set_col_kind(mip, col_idx, GLP_BV);
      glp_set_col_name(mip, col_idx,
          ("x_" + std::to_string(vehl_id) + "_" + std::to_string(shtrip_id)).c_str());
    }
  }
  for (const auto& cust_id : matchable_custs) {
    col_idx++;
    colmap[col_idx] = {cust_id, -1};
    int penalty = (unassign_penalty > 0 ? unassign_penalty : Cargo::basecost(cust_id));
    glp_set_obj_coef(mip, col_idx, penalty);
    glp_set_col_kind(mip, col_idx, GLP_BV);
    glp_set_col_name(mip, col_idx,
                     ("y_" + std::to_string(cust_id)).c_str());
  }
  }  // end populate coefficients

  /* Populate cells of the constraints matrix */
  std::vector<int>    ia(ncol * nrow + 1);  // <-- 1-indexed
  std::vector<int>    ja(ncol * nrow + 1);
  std::vector<double> ar(ncol * nrow + 1);
  /* For constraint 1) each vehicle serves 0 or 1 trips:
   *     ia[cel_idx] = row_idx
   *     ja[cel_idx] = col_idx
   *     ar[cel_idx] = 1 if the i of col matches the vehl of row_idx,
   *                   0 otherwise */
  size_t row_idx = 0;
  size_t cel_idx = 0;
  for (const auto& kv : vted_) {
    row_idx++;
    glp_set_row_bnds(mip, row_idx, GLP_UP, 0.0, 1.0);
    glp_set_row_name(mip, row_idx, ("v" + std::to_string(kv.first)).c_str());
    for (const auto& kv2 : colmap) {
      cel_idx++;
      ia[cel_idx] = row_idx;
      ja[cel_idx] = kv2.first;
      ar[cel_idx] = (kv.first == kv2.second.first ? 1 : 0);
    }
  }
  /* For constraint 2) each customer is either served or unserved:
   *     ia[cel_idx] = row_idx
   *     ja[cel_idx] = col_idx
   *     ar[cel_idx] = 1 if the j of col contains the cust of row_idx,
   *                   1 if the i of col equals cust.id(),
   *                   0 otherwise */
  for (const auto& cust_id : matchable_custs) {
    row_idx++;
    glp_set_row_bnds(mip, row_idx, GLP_FX, 1.0, 1.0);
    glp_set_row_name(mip, row_idx, ("c" + std::to_string(cust_id)).c_str());
    for (const auto& kv2 : colmap) {
      cel_idx++;
      ia[cel_idx] = row_idx;
      ja[cel_idx] = kv2.first;
      if (kv2.second.first == cust_id) ar[cel_idx] = 1;
      else if (kv2.second.second == -1)  ar[cel_idx] = 0;
      else {                             ar[cel_idx] = 0;
        for (const auto& cust2 : trip_.at(kv2.second.second))
          if (cust2.id() == cust_id) { ar[cel_idx] = 1;
            break;
          }
      }
    }
  }
  // end populate constraints matrix

  glp_load_matrix(mip, cel_idx, ia.data(), ja.data(), ar.data());
  glp_iocp cparams;
  glp_init_iocp(&cparams);
  cparams.presolve = GLP_ON;

  /* Heuristics */
  cparams.tm_lim = 15 * 1000;  // set time limit to 15 sec
  cparams.mip_gap = 0.001;     // set optimality gap to 0.1%

  // print(MessageType::Info) << "solving..." << std::endl;
  // int rc = glp_intopt(mip, &cparams);  // <-- solve!
  glp_intopt(mip, &cparams);  // <-- solve!

  // print << "mip return:" << std::endl;
  // switch (rc) {
  //   case 0: print << "good\n"; break;
  //   case GLP_EBOUND:  print(MessageType::Error) << "glp_ebound\n";  break;
  //   case GLP_EROOT:   print(MessageType::Error) << "glp_eroot\n";   break;
  //   case GLP_ENOPFS:  print(MessageType::Error) << "glp_enopfs\n";  break;
  //   case GLP_ENODFS:  print(MessageType::Error) << "glp_enodfs\n";  break;
  //   case GLP_EFAIL:   print(MessageType::Error) << "glp_efail\n";   break;
  //   case GLP_EMIPGAP: print(MessageType::Error) << "glp_emipgap\n"; break;
  //   case GLP_ETMLIM:  print(MessageType::Error) << "glp_etmlim\n";  break;
  //   case GLP_ESTOP:   print(MessageType::Error) << "glp_estop\n";   break;
  // }
  // int status = glp_mip_status(mip);
  // print << "mip status:" << std::endl;
  // switch (status) {
  //   case GLP_UNDEF:   print << "glp_undef\n";     break;
  //   case GLP_OPT:     print << "glp_opt\n";       break;
  //   case GLP_FEAS:    print << "glp_feas\n";      break;
  //   case GLP_NOFEAS:  print << "glp_nofeas\n";    break;
  // }

  // double z = glp_mip_obj_val(mip);
  // print << z << std::endl;

  if (this->done())
    return;

  /* Extract assignments from the results and commit to database */
  for (int i = 1; i <= ncol; ++i) {
    if (colmap[i].second == -1)
      continue;
    /* If the binary var for any other column is 1, then commit the
     * assignment. */
    if (glp_mip_col_val(mip, i) == 1) {
      MutableVehicle sync_vehl(vehmap.at(colmap[i].first));
      std::vector<Wayp>& new_rte = vt_rte.at(colmap[i].first).at(colmap[i].second);
      std::vector<Stop>& new_sch = vt_sch.at(colmap[i].first).at(colmap[i].second);
      std::vector<CustId> cadd {};
      for (const Customer& cust : trip_.at(colmap[i].second))
        cadd.push_back(cust.id());
      if (assign(cadd, {}, new_rte, new_sch, sync_vehl)) {
        for (const auto& cust : trip_.at(colmap[i].second)) {
          is_matched.at(cust.id()) = true;
          this->end_delay(cust.id());
        }
      } else {
        for (size_t i = 0; i < cadd.size(); ++i) {
          this->nrej_++;
          this->beg_delay(cadd.at(i));
        }
      }
    }
  }
  for (const auto& kv : is_matched)
    if (!kv.second)
      this->beg_delay(kv.first);

  glp_delete_prob(mip);
  }  // end mip (if vted_.size() > 0)

  this->end_ht();
}

void TripVehicleGrouping::handle_vehicle(const Vehicle& vehl) {
  this->grid_.insert(vehl);
}

void TripVehicleGrouping::end() {
  this->print_statistics();
}

void TripVehicleGrouping::listen(bool skip_assigned, bool skip_delayed) {
  grid_.clear();
  RSAlgorithm::listen(
    skip_assigned, skip_delayed);
}

bool TripVehicleGrouping::travel(const Vehicle& vehl,
                                 const std::vector<Customer>& custs,
                                 DistInt& cstout,
                                 std::vector<Stop>& schout,
                                 std::vector<Wayp>& rteout,
                                 GTree::G_Tree& gtre) {
  /* Need mutable copy so that schedule/route can be updated as customers
   * are added */
  MutableVehicle mtvehl = vehl;

  /* Containers */
  std::vector<Stop> schctr;
  std::vector<Wayp> rtectr;

  cstout = -1;

  /* Insert customers one by one */
  DistInt cstsum = 0;
  for (const Customer& cust : custs) {
    DistInt cst = sop_insert(mtvehl, cust, schctr, rtectr, gtre);
    if (chktw(schctr, rtectr)) {
      cstsum += cst;
      mtvehl.set_sch(schctr);
      mtvehl.set_rte(rtectr);
      mtvehl.reset_lvn();
    } else  // <-- a customer failed; trip cannot be served
      return false;
  }
  cstout = cstsum - mtvehl.route().cost();
  schout = schctr;
  rteout = rtectr;
  return true;
}

SharedTripId TripVehicleGrouping::add_trip(const SharedTrip& trip) {
  for (const auto& kv : trip_)
    if (kv.second == trip)
      return kv.first;
  stid_++;
  trip_[stid_] = trip;
  for (const auto& cust : trip)
    cted_[cust.id()].push_back(stid_);
  return stid_;
}

void TripVehicleGrouping::reset_workspace() {
  this->is_matched = {};
  this->rvgrph_rr_ = {};
  this->rvgrph_rv_ = {};
  this->rv_sch = {};
  this->rv_rte = {};
  this->rv_cst = {};
  this->matchable_custs = {};
  this->timeout_ = BATCH/2*1000;
  this->stid_ = 0;
  this->trip_ = {};
  this->vted_ = {};
  this->cted_ = {};
  this->vt_sch = {};
  this->vt_rte = {};
  this->vehmap = {};
}

int main() {
  Options option;
  option.path_to_roadnet  = "../../data/roadnetwork/bj5.rnet";
  option.path_to_gtree    = "../../data/roadnetwork/bj5.gtree";
  option.path_to_edges    = "../../data/roadnetwork/bj5.edges";
  option.path_to_problem  = "../../data/benchmark/rs-md-7.instance";
  option.path_to_solution = "trip_vehicle_grouping.sol";
  option.path_to_dataout  = "trip_vehicle_grouping.dat";
  option.time_multiplier  = 1;
  option.vehicle_speed    = 20;
  option.matching_period  = 60;
  option.static_mode = false;
  Cargo cargo(option);
  TripVehicleGrouping tvg;
  tvg.unassign_penalty = 1000000;
  cargo.start(tvg);

  return 0;
}

