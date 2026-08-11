#include "TripComputer.h"
#include "config.h"

TripComputer trip;

// Gasoline: L/h = MAF(g/s) * 3600 / (AFR * density) * trim
// Uses Harrier vehicle profile (Torque-style).
float TripComputer::mafToLph(float mafGps) {
  return mafGps * 3600.0f / (VEHICLE_AFR * VEHICLE_FUEL_DENSITY) * VEHICLE_FUEL_TRIM;
}

void TripComputer::begin() {
  prefs_.begin("trip", false);
  load();
  lastMs_ = millis();
  lastSaveMs_ = lastMs_;
  Serial.printf("[TRIP] loaded dist=%.3f km fuel=%.3f L tank=%.2f L\n",
                distanceKm_, fuelL_, fuelRemainingL_);
  Serial.printf("[VEH] %s %s %.1fL VE=%.0f%% tank=%.0fL\n",
                VEHICLE_NAME, VEHICLE_ENGINE, VEHICLE_DISPLACEMENT_L,
                VEHICLE_VE_PCT, TANK_CAPACITY_L);
}

void TripComputer::load() {
  distanceKm_ = prefs_.getFloat("dist", 0.0f);
  fuelL_ = prefs_.getFloat("fuel", 0.0f);
  fuelRemainingL_ = prefs_.getFloat("tankL", TANK_CAPACITY_L);
  if (distanceKm_ < 0 || distanceKm_ > 1.0e6f) distanceKm_ = 0;
  if (fuelL_ < 0 || fuelL_ > 1.0e5f) fuelL_ = 0;
  if (fuelRemainingL_ < 0 || fuelRemainingL_ > TANK_CAPACITY_L * 1.2f) {
    fuelRemainingL_ = TANK_CAPACITY_L;
  }
}

void TripComputer::save() {
  prefs_.putFloat("dist", distanceKm_);
  prefs_.putFloat("fuel", fuelL_);
  prefs_.putFloat("tankL", fuelRemainingL_);
  Serial.printf("[TRIP] saved dist=%.3f km fuel=%.3f L tank=%.2f L\n",
                distanceKm_, fuelL_, fuelRemainingL_);
}

void TripComputer::reset() {
  distanceKm_ = 0;
  fuelL_ = 0;
  instantL100_ = NAN;
  save();
  Serial.println("[TRIP] reset");
}

void TripComputer::setFuelRemainingL(float liters) {
  if (liters < 0) liters = 0;
  if (liters > TANK_CAPACITY_L) liters = TANK_CAPACITY_L;
  fuelRemainingL_ = liters;
  save();
  Serial.printf("[TRIP] fuel remaining set to %.2f L\n", fuelRemainingL_);
}

void TripComputer::fillTank() {
  setFuelRemainingL(TANK_CAPACITY_L);
}

float TripComputer::fuelRemainingPct() const {
  if (TANK_CAPACITY_L <= 0.0f) return NAN;
  return (fuelRemainingL_ / TANK_CAPACITY_L) * 100.0f;
}

float TripComputer::avgLPer100() const {
  if (distanceKm_ < TRIP_MIN_KM) return NAN;
  return (fuelL_ / distanceKm_) * 100.0f;
}

void TripComputer::update(const ObdData& data, bool mockMode) {
  const unsigned long now = millis();
  float dt = (now - lastMs_) / 1000.0f;
  if (lastMs_ == 0 || dt <= 0 || dt > 5.0f) {
    lastMs_ = now;
    return;
  }
  lastMs_ = now;

  // Mock must never pollute real trip totals (same NVS as live OBD).
  if (mockMode) {
    fuelLph_ = NAN;
    instantL100_ = NAN;
    return;
  }

  // Fuel rate: prefer PID 015E, else MAF via vehicle profile
  if (!isnan(data.fuelRateLph) && data.fuelRateLph >= 0) {
    fuelLph_ = data.fuelRateLph * VEHICLE_FUEL_TRIM;
  } else if (!isnan(data.mafGps) && data.mafGps > 0) {
    fuelLph_ = mafToLph(data.mafGps);
  } else {
    fuelLph_ = NAN;
  }

  // Key ON but engine stopped: MAF/ECU can still chatter — ignore fuel.
  const bool engineOn = !isnan(data.rpm) && data.rpm >= TRIP_MIN_RPM;
  const bool moving = !isnan(data.speedKmh) && data.speedKmh >= 1.0f;

  if (engineOn && !isnan(fuelLph_) && fuelLph_ > 0) {
    const float used = fuelLph_ * (dt / 3600.0f);
    fuelL_ += used;
    fuelRemainingL_ -= used;
    if (fuelRemainingL_ < 0) fuelRemainingL_ = 0;
  } else if (!engineOn) {
    fuelLph_ = 0;
  }

  if (moving) {
    distanceKm_ += data.speedKmh * (dt / 3600.0f);
    haveSpeed_ = true;
  }

  if (engineOn && moving && !isnan(fuelLph_) && data.speedKmh > 1.0f) {
    // L/100km = (L/h) / (km/h) * 100
    instantL100_ = (fuelLph_ / data.speedKmh) * 100.0f;
  } else {
    instantL100_ = NAN;
  }

  if (now - lastSaveMs_ >= TRIP_SAVE_MS) {
    lastSaveMs_ = now;
    save();
  }
}
