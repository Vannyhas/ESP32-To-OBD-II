#include "TripComputer.h"
#include "config.h"

TripComputer trip;

// Gasoline approx: L/h = MAF(g/s) * 3600 / (AFR * density)
// AFR≈14.7, density≈720 g/L → factor ≈ 0.339
float TripComputer::mafToLph(float mafGps) {
  return mafGps * 3600.0f / (14.7f * 720.0f);
}

void TripComputer::begin() {
  prefs_.begin("trip", false);
  load();
  lastMs_ = millis();
  lastSaveMs_ = lastMs_;
  Serial.printf("[TRIP] loaded dist=%.3f km fuel=%.3f L\n", distanceKm_, fuelL_);
}

void TripComputer::load() {
  distanceKm_ = prefs_.getFloat("dist", 0.0f);
  fuelL_ = prefs_.getFloat("fuel", 0.0f);
  if (distanceKm_ < 0 || distanceKm_ > 1.0e6f) distanceKm_ = 0;
  if (fuelL_ < 0 || fuelL_ > 1.0e5f) fuelL_ = 0;
}

void TripComputer::save() {
  prefs_.putFloat("dist", distanceKm_);
  prefs_.putFloat("fuel", fuelL_);
  Serial.printf("[TRIP] saved dist=%.3f km fuel=%.3f L\n", distanceKm_, fuelL_);
}

void TripComputer::reset() {
  distanceKm_ = 0;
  fuelL_ = 0;
  instantL100_ = NAN;
  save();
  Serial.println("[TRIP] reset");
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

  // Fuel rate: prefer PID 015E, else MAF
  if (!isnan(data.fuelRateLph) && data.fuelRateLph >= 0) {
    fuelLph_ = data.fuelRateLph;
  } else if (!isnan(data.mafGps) && data.mafGps > 0) {
    fuelLph_ = mafToLph(data.mafGps);
  } else {
    fuelLph_ = NAN;
  }

  // Key ON but engine stopped: MAF/ECU can still chatter — ignore fuel.
  const bool engineOn = !isnan(data.rpm) && data.rpm >= TRIP_MIN_RPM;
  const bool moving = !isnan(data.speedKmh) && data.speedKmh >= 1.0f;

  if (engineOn && !isnan(fuelLph_) && fuelLph_ > 0) {
    fuelL_ += fuelLph_ * (dt / 3600.0f);
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
