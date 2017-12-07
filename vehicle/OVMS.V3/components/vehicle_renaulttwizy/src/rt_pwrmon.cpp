/**
 * Project:      Open Vehicle Monitor System
 * Module:       Renault Twizy power monitor
 * 
 * (c) 2017  Michael Balzer <dexter@dexters-web.de>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "ovms_log.h"
static const char *TAG = "v-renaulttwizy";

#include <math.h>

#include "rt_pwrmon.h"
#include "metrics_standard.h"

#include "vehicle_renaulttwizy.h"


void vehicle_twizy_power(int verbosity, OvmsWriter* writer, OvmsCommand* cmd, int argc, const char* const* argv)
{
  OvmsVehicleRenaultTwizy* twizy = (OvmsVehicleRenaultTwizy*) MyVehicleFactory.ActiveVehicle();
  string type = StdMetrics.ms_v_type->AsString();
  
  if (!twizy || type != "RT")
  {
    writer->puts("Error: Twizy vehicle module not selected");
    return;
  }

  twizy->CommandPower(verbosity, writer, cmd, argc, argv);
}


/**
 * PowerInit:
 */
void OvmsVehicleRenaultTwizy::PowerInit()
{
  ESP_LOGI(TAG, "pwrmon subsystem init");
  
  // init metrics
  
  twizy_speedpwr[0].InitMetrics("x.rt.p.stats.cst.", Kph);
  twizy_speedpwr[1].InitMetrics("x.rt.p.stats.acc.", KphPS);
  twizy_speedpwr[2].InitMetrics("x.rt.p.stats.dec.", KphPS);
  
  twizy_levelpwr[0].InitMetrics("x.rt.p.stats.lup.");
  twizy_levelpwr[1].InitMetrics("x.rt.p.stats.ldn.");
  
  // init commands
  
  cmd_power = cmd_xrt->RegisterCommand("power", "Power/energy info", NULL, "", 0, 0, true);
  {
    cmd_power->RegisterCommand("report", "Trip efficiency report", vehicle_twizy_power, "", 0, 0, true);
    cmd_power->RegisterCommand("totals", "Power totals", vehicle_twizy_power, "", 0, 0, true);
    cmd_power->RegisterCommand("stats", "Generate RT-PWR-Stats entry", vehicle_twizy_power, "", 0, 0, true);
  }
  
}


/**
 * PowerUpdate:
 */
void OvmsVehicleRenaultTwizy::PowerUpdate()
{
  PowerCollectData();
  
  // publish metrics:
  
  unsigned long pwr;
  
  *StdMetrics.ms_v_bat_power = (float) twizy_power * 64 / 10000;
  
  pwr = twizy_speedpwr[CAN_SPEED_CONST].use
      + twizy_speedpwr[CAN_SPEED_ACCEL].use
      + twizy_speedpwr[CAN_SPEED_DECEL].use;
  
  *StdMetrics.ms_v_bat_energy_used = (float) pwr / WH_DIV / 1000;
  
  pwr = twizy_speedpwr[CAN_SPEED_CONST].rec
      + twizy_speedpwr[CAN_SPEED_ACCEL].rec
      + twizy_speedpwr[CAN_SPEED_DECEL].rec;
  
  *StdMetrics.ms_v_bat_energy_recd = (float) pwr / WH_DIV / 1000;
  
  for (speedpwr &stats : twizy_speedpwr)
    stats.UpdateMetrics();
  
  for (levelpwr &stats : twizy_levelpwr)
    stats.UpdateMetrics();
  
}


/**
 * PowerIsModified: get & clear metrics modification flags
 */
bool OvmsVehicleRenaultTwizy::PowerIsModified()
{
  bool modified =
    StdMetrics.ms_v_bat_power->IsModifiedAndClear(m_modifier) ||
    StdMetrics.ms_v_bat_energy_used->IsModifiedAndClear(m_modifier) ||
    StdMetrics.ms_v_bat_energy_recd->IsModifiedAndClear(m_modifier);
  
  for (speedpwr &stats : twizy_speedpwr)
    modified |= stats.IsModified(m_modifier);
  
  for (levelpwr &stats : twizy_levelpwr)
    modified |= stats.IsModified(m_modifier);
  
  return modified;
}


/**
 * PowerCollectData:
 */
void OvmsVehicleRenaultTwizy::PowerCollectData()
{
  long dist;
  int alt_diff, grade_perc;
  unsigned long coll_use, coll_rec;
  
  bool car_stale_gps = StdMetrics.ms_v_pos_gpslock->IsStale();
  int car_altitude = StdMetrics.ms_v_pos_altitude->AsInt();
  
  if ((twizy_status & CAN_STATUS_KEYON) == 0)
  {
    // CAR is turned off
    return;
  }
  else if (car_stale_gps == 0)
  {
    // no GPS for 2 minutes: reset section
    twizy_level_odo = 0;
    twizy_level_alt = 0;
    
    return;
  }
  else if (twizy_level_odo == 0)
  {
    // start new section:
    
    twizy_level_odo = twizy_odometer;
    twizy_level_alt = car_altitude;
    
    twizy_level_use = 0;
    twizy_level_rec = 0;
    
    return;
  }
  
  // calc section length:
  dist = (twizy_odometer - twizy_level_odo) * 10;
  if (dist < CAN_LEVEL_MINSECTLEN)
  {
    // section too short to collect
    return;
  }
  
  // OK, read + reset collected power sums:
  coll_use = twizy_level_use;
  coll_rec = twizy_level_rec;
  twizy_level_use = 0;
  twizy_level_rec = 0;
  
  // calc grade in percent:
  alt_diff = car_altitude - twizy_level_alt;
  grade_perc = (long) alt_diff * 100L / (long) dist;
  
  // set new section reference:
  twizy_level_odo = twizy_odometer;
  twizy_level_alt = car_altitude;
  
  // collect:
  if (grade_perc >= CAN_LEVEL_THRESHOLD)
  {
    twizy_levelpwr[CAN_LEVEL_UP].dist += dist; // in m
    twizy_levelpwr[CAN_LEVEL_UP].hsum += alt_diff; // in m
    twizy_levelpwr[CAN_LEVEL_UP].use += coll_use;
    twizy_levelpwr[CAN_LEVEL_UP].rec += coll_rec;
  }
  else if (grade_perc <= -CAN_LEVEL_THRESHOLD)
  {
    twizy_levelpwr[CAN_LEVEL_DOWN].dist += dist; // in m
    twizy_levelpwr[CAN_LEVEL_DOWN].hsum += -alt_diff; // in m
    twizy_levelpwr[CAN_LEVEL_DOWN].use += coll_use;
    twizy_levelpwr[CAN_LEVEL_DOWN].rec += coll_rec;
  }
  
}


/**
 * PowerReset:
 */
void OvmsVehicleRenaultTwizy::PowerReset()
{
  ESP_LOGD(TAG, "pwrmon reset");
  
  for (speedpwr &stats : twizy_speedpwr)
  {
    stats.dist = 0;
    stats.use = 0;
    stats.rec = 0;
    stats.spdsum = 0;
    stats.spdcnt = 0;
  }
  
  for (levelpwr &stats : twizy_levelpwr)
  {
    stats.dist = 0;
    stats.use = 0;
    stats.rec = 0;
    stats.hsum = 0;
  }
  
  twizy_speed_state = CAN_SPEED_CONST;
  twizy_speed_distref = twizy_dist;
  twizy_level_use = 0;
  twizy_level_rec = 0;
  twizy_charge_use = 0;
  twizy_charge_rec = 0;
  
  twizy_level_odo = 0;
  twizy_level_alt = 0;
  
  twizy_cc_charge = 0;
  twizy_cc_soc = 0;
  twizy_cc_power_level = 0;
}




#define PNAME(name) strdup((prefix + name).c_str())

void speedpwr::InitMetrics(const string prefix, metric_unit_t spdunit)
{
  m_dist = MyMetrics.InitFloat(PNAME("dist"), SM_STALE_HIGH, 0, Kilometers);
  m_used = MyMetrics.InitFloat(PNAME("used"), SM_STALE_HIGH, 0, kWh);
  m_recd = MyMetrics.InitFloat(PNAME("recd"), SM_STALE_HIGH, 0, kWh);
  m_spdavg = MyMetrics.InitFloat(PNAME("spdavg"), SM_STALE_HIGH, 0, spdunit);
  m_spdunit = spdunit;
}

void speedpwr::UpdateMetrics()
{
  *m_dist = (float) dist / 10000;
  *m_used = (float) use / WH_DIV / 1000;
  *m_recd = (float) rec / WH_DIV / 1000;
  if (spdcnt > 0)
    *m_spdavg = (float) (spdsum / spdcnt) / ((m_spdunit==Kph) ? 100 : 10);
  else
    *m_spdavg = (float) 0;
}

bool speedpwr::IsModified(size_t m_modifier)
{
  bool modified =
    m_dist->IsModifiedAndClear(m_modifier) ||
    m_used->IsModifiedAndClear(m_modifier) ||
    m_recd->IsModifiedAndClear(m_modifier) ||
    m_spdavg->IsModifiedAndClear(m_modifier);
  return modified;
}


void levelpwr::InitMetrics(const string prefix)
{
  m_dist = MyMetrics.InitFloat(PNAME("dist"), SM_STALE_HIGH, 0, Kilometers);
  m_hsum = MyMetrics.InitFloat(PNAME("hsum"), SM_STALE_HIGH, 0, Meters);
  m_used = MyMetrics.InitFloat(PNAME("used"), SM_STALE_HIGH, 0, kWh);
  m_recd = MyMetrics.InitFloat(PNAME("recd"), SM_STALE_HIGH, 0, kWh);
}

void levelpwr::UpdateMetrics()
{
  *m_dist = (float) dist / 1000;
  *m_hsum = (float) hsum;
  *m_used = (float) use / WH_DIV / 1000;
  *m_recd = (float) rec / WH_DIV / 1000;
}

bool levelpwr::IsModified(size_t m_modifier)
{
  bool modified =
    m_dist->IsModifiedAndClear(m_modifier) ||
    m_used->IsModifiedAndClear(m_modifier) ||
    m_recd->IsModifiedAndClear(m_modifier) ||
    m_hsum->IsModifiedAndClear(m_modifier);
  return modified;
}


/**
 * CommandPower: power report|totals|stats
 *  totals: output current totals (text notification)
 *  report: output trip efficiency report (text notification)
 *  stats: output history entry RT-PWR-Stats
 */
OvmsVehicleRenaultTwizy::vehicle_command_t OvmsVehicleRenaultTwizy::CommandPower(int verbosity, OvmsWriter* writer, OvmsCommand* cmd, int argc, const char* const* argv)
{
  UINT8 i;
  
  ESP_LOGV(TAG, "command power %s, verbosity=%d", cmd->GetName(), verbosity);
  
  if (strcmp(cmd->GetName(), "stats") == 0)
  {
    /* Output power usage statistics:
     *
     * RT-PWR-Stats,0,86400
     *  ,<speed_CONST_dist>,<speed_CONST_use>,<speed_CONST_rec>
     *  ,<speed_ACCEL_dist>,<speed_ACCEL_use>,<speed_ACCEL_rec>
     *  ,<speed_DECEL_dist>,<speed_DECEL_use>,<speed_DECEL_rec>
     *  ,<level_UP_dist>,<level_UP_hsum>,<level_UP_use>,<level_UP_rec>
     *  ,<level_DOWN_dist>,<level_DOWN_hsum>,<level_DOWN_use>,<level_DOWN_rec>
     *  ,<speed_CONST_cnt>,<speed_CONST_sum>
     *  ,<speed_ACCEL_cnt>,<speed_ACCEL_sum>
     *  ,<speed_DECEL_cnt>,<speed_DECEL_sum>
     *  ,<charge_used>,<charge_recd>
     *
     * (cnt = 1/10 seconds, CONST_sum = speed, other sum = delta)
     */
    
    if (verbosity < 200)
      return Fail;
    
    writer->printf("RT-PWR-Stats,0,86400");
    
    for (i=0; i<=2; i++)
    {
      writer->printf(",%lu,%lu,%lu,%lu,%lu",
        twizy_speedpwr[i].dist / 10,
        twizy_speedpwr[i].use / WH_DIV,
        twizy_speedpwr[i].rec / WH_DIV,
        twizy_speedpwr[i].spdcnt,
        twizy_speedpwr[i].spdsum);
    }
    
    for (i=0; i<=1; i++)
    {
      writer->printf(",%lu,%u,%lu,%lu",
        twizy_levelpwr[i].dist,
        twizy_levelpwr[i].hsum,
        twizy_levelpwr[i].use / WH_DIV,
        twizy_levelpwr[i].rec / WH_DIV);
    }
    
    writer->printf(",%.2f,%.2f\n",
      (float) twizy_charge_use / AH_DIV,
      (float) twizy_charge_rec / AH_DIV);
    
    return Success;
  }
  
  
  // Gather common data for text reports:
  
  unsigned long pwr_dist, pwr_use, pwr_rec, odo_dist;
  UINT8 prc_const = 0, prc_accel = 0, prc_decel = 0;

  // overall sums:
  pwr_dist = twizy_speedpwr[CAN_SPEED_CONST].dist
          + twizy_speedpwr[CAN_SPEED_ACCEL].dist
          + twizy_speedpwr[CAN_SPEED_DECEL].dist;

  pwr_use = twizy_speedpwr[CAN_SPEED_CONST].use
          + twizy_speedpwr[CAN_SPEED_ACCEL].use
          + twizy_speedpwr[CAN_SPEED_DECEL].use;

  pwr_rec = twizy_speedpwr[CAN_SPEED_CONST].rec
          + twizy_speedpwr[CAN_SPEED_ACCEL].rec
          + twizy_speedpwr[CAN_SPEED_DECEL].rec;
  
  odo_dist = twizy_odometer - twizy_odometer_tripstart;

  // distribution:
  if (pwr_dist > 0)
  {
    prc_const = (twizy_speedpwr[CAN_SPEED_CONST].dist * 1000 / pwr_dist + 5) / 10;
    prc_accel = (twizy_speedpwr[CAN_SPEED_ACCEL].dist * 1000 / pwr_dist + 5) / 10;
    prc_decel = (twizy_speedpwr[CAN_SPEED_DECEL].dist * 1000 / pwr_dist + 5) / 10;
  }
  
  if ((pwr_use == 0) || (pwr_dist <= 10))
  {
    // not driven: only output power totals:
    writer->printf("Power -%lu +%lu Wh\n",
      (pwr_use + WH_RND) / WH_DIV,
      (pwr_rec + WH_RND) / WH_DIV);
  }
  
  else if (strcmp(cmd->GetName(), "totals") == 0)
  {
    /* Output power totals:
     * 
     * Template:
     *    Power -1234 +1234 Wh 123.4 km
     *    Const 12% -1234 +1234 Wh
     *    Accel 12% -1234 +1234 Wh
     *    Decel 12% -1234 +1234 Wh
     *    Up 1234m -1234 +1234 Wh
     *    Down 1234m -1234 +1234 Wh
     * 
     * Size:
     *  30 + 3*25 + 24 + 26 → 155
     *  COMMAND_RESULT_MINIMAL: omit Up/Down → 105
     */
    
    if (verbosity >= COMMAND_RESULT_MINIMAL)
    {
      writer->printf(
        "Power -%lu +%lu Wh %.1f km\n"
        "Const %d%% -%lu +%lu Wh\n"
        "Accel %d%% -%lu +%lu Wh\n"
        "Decel %d%% -%lu +%lu Wh\n",
        (pwr_use + WH_RND) / WH_DIV,
        (pwr_rec + WH_RND) / WH_DIV,
        (float) odo_dist / 100,
        prc_const,
        (twizy_speedpwr[CAN_SPEED_CONST].use + WH_RND) / WH_DIV,
        (twizy_speedpwr[CAN_SPEED_CONST].rec + WH_RND) / WH_DIV,
        prc_accel,
        (twizy_speedpwr[CAN_SPEED_ACCEL].use + WH_RND) / WH_DIV,
        (twizy_speedpwr[CAN_SPEED_ACCEL].rec + WH_RND) / WH_DIV,
        prc_decel,
        (twizy_speedpwr[CAN_SPEED_DECEL].use + WH_RND) / WH_DIV,
        (twizy_speedpwr[CAN_SPEED_DECEL].rec + WH_RND) / WH_DIV);
    }
    
    if (verbosity >= COMMAND_RESULT_SMS)
    {
      writer->printf(
        "Up %lum -%lu +%lu Wh\n"
        "Down %lum -%lu +%lu Wh\n",
        twizy_levelpwr[CAN_LEVEL_UP].hsum,
        (twizy_levelpwr[CAN_LEVEL_UP].use + WH_RND) / WH_DIV,
        (twizy_levelpwr[CAN_LEVEL_UP].rec + WH_RND) / WH_DIV,
        twizy_levelpwr[CAN_LEVEL_DOWN].hsum,
        (twizy_levelpwr[CAN_LEVEL_DOWN].use + WH_RND) / WH_DIV,
        (twizy_levelpwr[CAN_LEVEL_DOWN].rec + WH_RND) / WH_DIV);
    }
    
  }
  
  else
  {
    /* Output power efficiency trip report (default):
     * 
     * Template:
     *    Trip 12.3km 12.3kph 123Wpk/12% SOC-12.3%=12.3%
     *    === 12% 123Wpk/12%
     *    +++ 12% 12.3kps 123Wpk/12%
     *    --- 12% 12.3kps 123Wpk/12%
     *    ^^^ 123m 123Wpk/12%
     *    vvv 123m 123Wpk/12%
     * 
     * Size:
     *  47 + 19 + 27 + 27 + 20 + 20 → 160
     *  COMMAND_RESULT_MINIMAL: omit ^^^/vvv lines → 120
     */
    
    long pwr;
    long dist;

    // speed distances are in ~ 1/10 m based on cyclic counter in ID 59E
    // real distances per odometer (10 m resolution) are ~ 8-9% lower
    // compensate:
    
    float correction = ((float) odo_dist * 100) / pwr_dist;
    
    if (verbosity >= COMMAND_RESULT_MINIMAL)
    {
      writer->printf("Trip %.1fkm %.1fkph",
        (float) odo_dist / 100,
        (float) ((twizy_speedpwr[CAN_SPEED_CONST].spdcnt > 0)
            ? (twizy_speedpwr[CAN_SPEED_CONST].spdsum / twizy_speedpwr[CAN_SPEED_CONST].spdcnt) / 100
            : 0));
      
      dist = (float) pwr_dist * correction;
      pwr = pwr_use - pwr_rec;
      if ((pwr_use > 0) && (dist > 0))
      {
        writer->printf(" %ldWpk/%d%%",
          (pwr / dist * 10000 + ((pwr>=0)?WH_RND:-WH_RND)) / WH_DIV,
          (pwr_rec / (pwr_use/1000) + 5) / 10);
      }
      
      writer->printf(" SOC%+.1f%%=%.1f%%",
        (float) (twizy_soc - twizy_soc_tripstart) / 100,
        (float) twizy_soc / 100);
      
      // === 12% 123Wpk/12%
      pwr_use = twizy_speedpwr[CAN_SPEED_CONST].use;
      pwr_rec = twizy_speedpwr[CAN_SPEED_CONST].rec;
      dist = (float) twizy_speedpwr[CAN_SPEED_CONST].dist * correction;
      pwr = pwr_use - pwr_rec;
      if ((pwr_use > 0) && (dist > 0))
      {
        writer->printf("\n=== %d%% %ldWpk/%d%%",
          prc_const,
          (pwr / dist * 10000 + ((pwr>=0)?WH_RND:-WH_RND)) / WH_DIV,
          (pwr_rec / (pwr_use/1000) + 5) / 10);
      }

      // +++ 12% 12.3kps 123Wpk/12%
      pwr_use = twizy_speedpwr[CAN_SPEED_ACCEL].use;
      pwr_rec = twizy_speedpwr[CAN_SPEED_ACCEL].rec;
      dist = (float) twizy_speedpwr[CAN_SPEED_ACCEL].dist * correction;
      pwr = pwr_use - pwr_rec;
      if ((pwr_use > 0) && (dist > 0))
      {
        writer->printf("\n+++ %d%% %.1fkps %ldWpk/%d%%",
          prc_accel,
          (float) ((twizy_speedpwr[CAN_SPEED_ACCEL].spdcnt > 0)
              ? ((twizy_speedpwr[CAN_SPEED_ACCEL].spdsum * 10) / twizy_speedpwr[CAN_SPEED_ACCEL].spdcnt + 50) / 100
              : 0),
          (pwr / dist * 10000 + ((pwr>=0)?WH_RND:-WH_RND)) / WH_DIV,
          (pwr_rec / (pwr_use/1000) + 5) / 10);
      }

      // --- 12% 12.3kps 123Wpk/12%
      pwr_use = twizy_speedpwr[CAN_SPEED_DECEL].use;
      pwr_rec = twizy_speedpwr[CAN_SPEED_DECEL].rec;
      dist = (float) twizy_speedpwr[CAN_SPEED_DECEL].dist * correction;
      pwr = pwr_use - pwr_rec;
      if ((pwr_use > 0) && (dist > 0))
      {
        writer->printf("\n--- %d%% %.1fkps %ldWpk/%d%%",
          prc_decel,
          (float) ((twizy_speedpwr[CAN_SPEED_DECEL].spdcnt > 0)
              ? ((twizy_speedpwr[CAN_SPEED_DECEL].spdsum * 10) / twizy_speedpwr[CAN_SPEED_DECEL].spdcnt + 50) / 100
              : 0),
          (pwr / dist * 10000 + ((pwr>=0)?WH_RND:-WH_RND)) / WH_DIV,
          (pwr_rec / (pwr_use/1000) + 5) / 10);
      }
      
    } // if (verbosity >= COMMAND_RESULT_MINIMAL)
    
    if (verbosity >= COMMAND_RESULT_SMS)
    {
      // level distances are in 1 m and based on odometer
      // (no correction necessary)

      // ^^^ 123m 123Wpk/12%
      pwr_use = twizy_levelpwr[CAN_LEVEL_UP].use;
      pwr_rec = twizy_levelpwr[CAN_LEVEL_UP].rec;
      dist = twizy_levelpwr[CAN_LEVEL_UP].dist;
      pwr = pwr_use - pwr_rec;
      if ((pwr_use > 0) && (dist > 0))
      {
        writer->printf("\n^^^ %dm %ldWpk/%d%%",
          twizy_levelpwr[CAN_LEVEL_UP].hsum,
          (pwr / dist * 1000 + ((pwr>=0)?WH_RND:-WH_RND)) / WH_DIV,
          (pwr_rec / (pwr_use/1000) + 5) / 10);
      }

      // vvv 123m 123Wpk/12%
      pwr_use = twizy_levelpwr[CAN_LEVEL_DOWN].use;
      pwr_rec = twizy_levelpwr[CAN_LEVEL_DOWN].rec;
      dist = twizy_levelpwr[CAN_LEVEL_DOWN].dist;
      pwr = pwr_use - pwr_rec;
      if ((pwr_use > 0) && (dist > 0))
      {
        writer->printf("\nvvv %dm %ldWpk/%d%%",
          twizy_levelpwr[CAN_LEVEL_DOWN].hsum,
          (pwr / dist * 1000 + ((pwr>=0)?WH_RND:-WH_RND)) / WH_DIV,
          (pwr_rec / (pwr_use/1000) + 5) / 10);
      }
    
    } // if (verbosity >= COMMAND_RESULT_SMS)
    
    writer->puts("");
    
  } // Output power efficiency trip report
  
  return Success;
}


