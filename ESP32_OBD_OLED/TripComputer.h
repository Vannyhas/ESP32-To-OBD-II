#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "Elm327.h"

// Trip computer: integrates distance from speed and fuel from MAF / fuel-rate.
// Persists totals in NVS (survives reboot).
class TripComputer {
 public:
  void begin();
  void update(const ObdData& data, bool mockMode);
  void reset();
  void save();

  float distanceKm() const { return distanceKm_; }
  float fuelLiters() const { return fuelL_; }
  // L/100km average; NAN until enough distance
  float avgLPer100() const;
  float instantLPer100() const { return instantL100_; }
  float fuelLPerHour() const { return fuelLph_; }

 private:
  void load();
  static float mafToLph(float mafGps);

  Preferences prefs_;
  float distanceKm_ = 0;
  float fuelL_ = 0;
  float fuelLph_ = NAN;
  float instantL100_ = NAN;
  unsigned long lastMs_ = 0;
  unsigned long lastSaveMs_ = 0;
  bool haveSpeed_ = false;
};

extern TripComputer trip;
