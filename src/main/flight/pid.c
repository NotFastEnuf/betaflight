/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <platform.h>

#include "build/build_config.h"
#include "build/debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "config/config_reset.h"
#include "pg/pg.h"
#include "pg/pg_ids.h"

#include "drivers/sound_beeper.h"
#include "drivers/time.h"

#include "fc/fc_core.h"
#include "fc/fc_rc.h"

#include "fc/rc_controls.h"
#include "fc/runtime_config.h"

#include "flight/pid.h"
#include "flight/imu.h"
#include "flight/mixer.h"

#include "io/gps.h"

#include "sensors/gyro.h"
#include "sensors/acceleration.h"

FAST_RAM uint32_t targetPidLooptime;
static FAST_RAM bool pidStabilisationEnabled;

static FAST_RAM bool inCrashRecoveryMode = false;

FAST_RAM float axisPID_P[3], axisPID_I[3], axisPID_D[3], axisPIDSum[3];

static FAST_RAM float dT;

PG_REGISTER_WITH_RESET_TEMPLATE(pidConfig_t, pidConfig, PG_PID_CONFIG, 1);

#ifdef STM32F10X
#define PID_PROCESS_DENOM_DEFAULT       1
#elif defined(USE_GYRO_SPI_MPU6000) || defined(USE_GYRO_SPI_MPU6500)  || defined(USE_GYRO_SPI_ICM20689)
#define PID_PROCESS_DENOM_DEFAULT       4
#else
#define PID_PROCESS_DENOM_DEFAULT       2
#endif

#ifdef USE_RUNAWAY_TAKEOFF
PG_RESET_TEMPLATE(pidConfig_t, pidConfig,
    .pid_process_denom = PID_PROCESS_DENOM_DEFAULT,
    .runaway_takeoff_prevention = true,
    .runaway_takeoff_threshold = 60,            // corresponds to a pidSum value of 60% (raw 600) in the blackbox viewer
    .runaway_takeoff_activate_delay = 75,       // 75ms delay before activation (pidSum above threshold time)
    .runaway_takeoff_deactivate_throttle = 25,  // throttle level % needed to accumulate deactivation time
    .runaway_takeoff_deactivate_delay = 500     // Accumulated time (in milliseconds) before deactivation in successful takeoff
);
#else
PG_RESET_TEMPLATE(pidConfig_t, pidConfig,
    .pid_process_denom = PID_PROCESS_DENOM_DEFAULT
);
#endif

PG_REGISTER_ARRAY_WITH_RESET_FN(pidProfile_t, MAX_PROFILE_COUNT, pidProfiles, PG_PID_PROFILE, 2);

void resetPidProfile(pidProfile_t *pidProfile)
{
    RESET_CONFIG(pidProfile_t, pidProfile,
        .pid = {
            [PID_ROLL] =  { 40, 40, 30 },
            [PID_PITCH] = { 58, 50, 35 },
            [PID_YAW] =   { 70, 45, 20 },
            [PID_ALT] =   { 50, 0, 0 },
            [PID_POS] =   { 15, 0, 0 },     // POSHOLD_P * 100, POSHOLD_I * 100,
            [PID_POSR] =  { 34, 14, 53 },   // POSHOLD_RATE_P * 10, POSHOLD_RATE_I * 100, POSHOLD_RATE_D * 1000,
            [PID_NAVR] =  { 25, 33, 83 },   // NAV_P * 10, NAV_I * 100, NAV_D * 1000
            [PID_LEVEL] = { 50, 50, 75 },
            [PID_MAG] =   { 40, 0, 0 },
            [PID_VEL] =   { 55, 55, 75 }
        },

        .pidSumLimit = PIDSUM_LIMIT,
        .pidSumLimitYaw = PIDSUM_LIMIT_YAW,
        .yaw_lpf_hz = 0,
        .dterm_lpf_hz = 100,    // filtering ON by default
        .dterm_notch_hz = 260,
        .dterm_notch_cutoff = 160,
        .dterm_filter_type = FILTER_BIQUAD,
        .itermWindupPointPercent = 50,
        .vbatPidCompensation = 0,
        .pidAtMinThrottle = PID_STABILISATION_ON,
        .levelAngleLimit = 65,
        .setpointRelaxRatio = 100,
        .dtermSetpointWeight = 0,
        .yawRateAccelLimit = 100,
        .rateAccelLimit = 0,
        .itermThrottleThreshold = 350,
        .itermAcceleratorGain = 1000,
        .crash_time = 500,          // ms
        .crash_delay = 0,           // ms
        .crash_recovery_angle = 10, // degrees
        .crash_recovery_rate = 100, // degrees/second
        .crash_dthreshold = 50,     // degrees/second/second
        .crash_gthreshold = 400,    // degrees/second
        .crash_setpoint_threshold = 350, // degrees/second
        .crash_recovery = PID_CRASH_RECOVERY_OFF, // off by default
        .horizon_tilt_effect = 130,
        .horizon_tilt_expert_mode = false,
        .crash_limit_yaw = 200,
        .itermLimit = 150
    );
}

void pgResetFn_pidProfiles(pidProfile_t *pidProfiles)
{
    for (int i = 0; i < MAX_PROFILE_COUNT; i++) {
        resetPidProfile(&pidProfiles[i]);
    }
}

static void pidSetTargetLooptime(uint32_t pidLooptime)
{
    targetPidLooptime = pidLooptime;
    dT = (float)targetPidLooptime * 0.000001f;
}

void pidResetITerm(void)
{
    for (int axis = 0; axis < 3; axis++) {
        axisPID_I[axis] = 0.0f;
    }
}

static FAST_RAM float itermAccelerator = 1.0f;

void pidSetItermAccelerator(float newItermAccelerator)
{
    itermAccelerator = newItermAccelerator;
}

void pidStabilisationState(pidStabilisationState_e pidControllerState)
{
    pidStabilisationEnabled = (pidControllerState == PID_STABILISATION_ON) ? true : false;
}

const angle_index_t rcAliasToAngleIndexMap[] = { AI_ROLL, AI_PITCH };

static FAST_RAM filterApplyFnPtr dtermNotchFilterApplyFn;
static FAST_RAM void *dtermFilterNotch[2];
static FAST_RAM filterApplyFnPtr dtermLpfApplyFn;
static FAST_RAM void *dtermFilterLpf[2];
static FAST_RAM filterApplyFnPtr ptermYawFilterApplyFn;
static FAST_RAM void *ptermYawFilter;

typedef union dtermFilterLpf_u {
    pt1Filter_t pt1Filter[2];
    biquadFilter_t biquadFilter[2];
    firFilterDenoise_t denoisingFilter[2];
} dtermFilterLpf_t;

void pidInitFilters(const pidProfile_t *pidProfile)
{
    BUILD_BUG_ON(FD_YAW != 2); // only setting up Dterm filters on roll and pitch axes, so ensure yaw axis is 2

    if (targetPidLooptime == 0) {
        // no looptime set, so set all the filters to null
        dtermNotchFilterApplyFn = nullFilterApply;
        dtermLpfApplyFn = nullFilterApply;
        ptermYawFilterApplyFn = nullFilterApply;
        return;
    }

    const uint32_t pidFrequencyNyquist = (1.0f / dT) / 2; // No rounding needed

    uint16_t dTermNotchHz;
    if (pidProfile->dterm_notch_hz <= pidFrequencyNyquist) {
        dTermNotchHz = pidProfile->dterm_notch_hz;
    } else {
        if (pidProfile->dterm_notch_cutoff < pidFrequencyNyquist) {
            dTermNotchHz = pidFrequencyNyquist;
        } else {
            dTermNotchHz = 0;
        }
    }

    if (dTermNotchHz != 0 && pidProfile->dterm_notch_cutoff != 0) {
        static biquadFilter_t biquadFilterNotch[2];
        dtermNotchFilterApplyFn = (filterApplyFnPtr)biquadFilterApply;
        const float notchQ = filterGetNotchQ(dTermNotchHz, pidProfile->dterm_notch_cutoff);
        for (int axis = FD_ROLL; axis <= FD_PITCH; axis++) {
            dtermFilterNotch[axis] = &biquadFilterNotch[axis];
            biquadFilterInit(dtermFilterNotch[axis], dTermNotchHz, targetPidLooptime, notchQ, FILTER_NOTCH);
        }
    } else {
        dtermNotchFilterApplyFn = nullFilterApply;
    }

    static dtermFilterLpf_t dtermFilterLpfUnion;
    if (pidProfile->dterm_lpf_hz == 0 || pidProfile->dterm_lpf_hz > pidFrequencyNyquist) {
        dtermLpfApplyFn = nullFilterApply;
    } else {
        switch (pidProfile->dterm_filter_type) {
        default:
            dtermLpfApplyFn = nullFilterApply;
            break;
        case FILTER_PT1:
            dtermLpfApplyFn = (filterApplyFnPtr)pt1FilterApply;
            for (int axis = FD_ROLL; axis <= FD_PITCH; axis++) {
                dtermFilterLpf[axis] = &dtermFilterLpfUnion.pt1Filter[axis];
                pt1FilterInit(dtermFilterLpf[axis], pidProfile->dterm_lpf_hz, dT);
            }
            break;
        case FILTER_BIQUAD:
            dtermLpfApplyFn = (filterApplyFnPtr)biquadFilterApply;
            for (int axis = FD_ROLL; axis <= FD_PITCH; axis++) {
                dtermFilterLpf[axis] = &dtermFilterLpfUnion.biquadFilter[axis];
                biquadFilterInitLPF(dtermFilterLpf[axis], pidProfile->dterm_lpf_hz, targetPidLooptime);
            }
            break;
        case FILTER_FIR:
            dtermLpfApplyFn = (filterApplyFnPtr)firFilterDenoiseUpdate;
            for (int axis = FD_ROLL; axis <= FD_PITCH; axis++) {
                dtermFilterLpf[axis] = &dtermFilterLpfUnion.denoisingFilter[axis];
                firFilterDenoiseInit(dtermFilterLpf[axis], pidProfile->dterm_lpf_hz, targetPidLooptime);
            }
            break;
        }
    }

    static pt1Filter_t pt1FilterYaw;
    if (pidProfile->yaw_lpf_hz == 0 || pidProfile->yaw_lpf_hz > pidFrequencyNyquist) {
        ptermYawFilterApplyFn = nullFilterApply;
    } else {
        ptermYawFilterApplyFn = (filterApplyFnPtr)pt1FilterApply;
        ptermYawFilter = &pt1FilterYaw;
        pt1FilterInit(ptermYawFilter, pidProfile->yaw_lpf_hz, dT);
    }
}

static FAST_RAM float Kp[3], Ki[3], Kd[3];
static FAST_RAM float maxVelocity[3];
static FAST_RAM float relaxFactor;
static FAST_RAM float dtermSetpointWeight;
static FAST_RAM float levelGain, horizonGain, horizonTransition, horizonCutoffDegrees, horizonFactorRatio;
static FAST_RAM float ITermWindupPointInv;
static FAST_RAM uint8_t horizonTiltExpertMode;
static FAST_RAM timeDelta_t crashTimeLimitUs;
static FAST_RAM timeDelta_t crashTimeDelayUs;
static FAST_RAM int32_t crashRecoveryAngleDeciDegrees;
static FAST_RAM float crashRecoveryRate;
static FAST_RAM float crashDtermThreshold;
static FAST_RAM float crashGyroThreshold;
static FAST_RAM float crashSetpointThreshold;
static FAST_RAM float crashLimitYaw;
static FAST_RAM float itermLimit;

void pidInitConfig(const pidProfile_t *pidProfile)
{
    for (int axis = FD_ROLL; axis <= FD_YAW; axis++) {
        Kp[axis] = PTERM_SCALE * pidProfile->pid[axis].P;
        Ki[axis] = ITERM_SCALE * pidProfile->pid[axis].I;
        Kd[axis] = DTERM_SCALE * pidProfile->pid[axis].D;
    }
    dtermSetpointWeight = pidProfile->dtermSetpointWeight / 127.0f;
    relaxFactor = 1.0f / (pidProfile->setpointRelaxRatio / 100.0f);
    levelGain = pidProfile->pid[PID_LEVEL].P / 10.0f;
    horizonGain = pidProfile->pid[PID_LEVEL].I / 10.0f;
    horizonTransition = (float)pidProfile->pid[PID_LEVEL].D;
    horizonTiltExpertMode = pidProfile->horizon_tilt_expert_mode;
    horizonCutoffDegrees = (175 - pidProfile->horizon_tilt_effect) * 1.8f;
    horizonFactorRatio = (100 - pidProfile->horizon_tilt_effect) * 0.01f;
    maxVelocity[FD_ROLL] = maxVelocity[FD_PITCH] = pidProfile->rateAccelLimit * 100 * dT;
    maxVelocity[FD_YAW] = pidProfile->yawRateAccelLimit * 100 * dT;
    const float ITermWindupPoint = (float)pidProfile->itermWindupPointPercent / 100.0f;
    ITermWindupPointInv = 1.0f / (1.0f - ITermWindupPoint);
    crashTimeLimitUs = pidProfile->crash_time * 1000;
    crashTimeDelayUs = pidProfile->crash_delay * 1000;
    crashRecoveryAngleDeciDegrees = pidProfile->crash_recovery_angle * 10;
    crashRecoveryRate = pidProfile->crash_recovery_rate;
    crashGyroThreshold = pidProfile->crash_gthreshold;
    crashDtermThreshold = pidProfile->crash_dthreshold;
    crashSetpointThreshold = pidProfile->crash_setpoint_threshold;
    crashLimitYaw = pidProfile->crash_limit_yaw;
    itermLimit = pidProfile->itermLimit;
}

void pidInit(const pidProfile_t *pidProfile)
{
    pidSetTargetLooptime(gyro.targetLooptime * pidConfig()->pid_process_denom); // Initialize pid looptime
    pidInitFilters(pidProfile);
    pidInitConfig(pidProfile);
}


void pidCopyProfile(uint8_t dstPidProfileIndex, uint8_t srcPidProfileIndex)
{
    if ((dstPidProfileIndex < MAX_PROFILE_COUNT-1 && srcPidProfileIndex < MAX_PROFILE_COUNT-1)
        && dstPidProfileIndex != srcPidProfileIndex
    ) {
        memcpy(pidProfilesMutable(dstPidProfileIndex), pidProfilesMutable(srcPidProfileIndex), sizeof(pidProfile_t));
    }
}


// calculates strength of RACEMODEhoriozon leveling and the strength and position range of RACEMODEangle leveling beyond levelAngleLimit; 0 = none, 1.0 = most leveling
static float calcHorizonLevelStrength(void)
{   //NFE NOTE - horizonTransition piece of horizonLevelStrength has been removed and horizonLevelStrength is set to inclinationLevelRatio for RACEMODE
	
    // start with 1.0 at center stick, 0.0 at max stick deflection:
    float horizonLevelStrength;
    // 0 at level, 90 at vertical, 180 at inverted (degrees):
    const float currentInclination = MAX(ABS(attitude.values.roll), ABS(attitude.values.pitch)) / 10.0f;
//*************************************************************************************************************************************************************************
//******************************************************************HAAAALLLLLLLP******************************************************************************************
//How do I call the level angle limit into this define
	// Used as a factor in the numerator of inclinationLevelRatio - this will cause the fade of leveling strength to start at levelAngleLimit for RACEMODEangle
	//const float racemodeTransitionFactor = horizonCutoffDegrees/(horizonCutoffDegrees-(pidProfile->levelAngleLimit))

    // horizonTiltExpertMode:  0 = RACEMODEangle - ANGLE LIMIT BEHAVIOUR ON ROLL AXIS
    //                         1 = RACEMODEhoriozon - HORIZON TYPE BEHAVIOUR ON ROLL AXIS
	
    if (horizonTiltExpertMode) { //determines the leveling strength of RACEMODEhoriozon
		
        if (horizonTransition > 0 && horizonCutoffDegrees > 0) { // if d_level > 0 and horizonTiltEffect < 175                    
		//causes leveling to fade from center stick to horizonCutoffDegrees	where leveling goes to zero	
		//horizonTransition (if this variable is open now) can be used to move the begining point of leveling fade off of zero stick - this needs work
        	const float inclinationLevelRatio = constrainf((horizonCutoffDegrees-currentInclination) / horizonCutoffDegrees, 0, 1);
            	// apply inclination ratio to horizonLevelStrength which lower leveling to zero as a function of angle regardless of stick position
           	 horizonLevelStrength = inclinationLevelRatio;
        } else  { // d_level=0 or horizon_tilt_effect>=175 means no leveling
         	 horizonLevelStrength = 0;	  		  
        }
		
    } else { // horizon_tilt_expert_mode = 0  determines the leveling strength and moves the leveling region of RACEMODEangle to the edge of levelAngleLimit
			
        if (horizonCutoffDegrees > 0) { //horizonTiltEffect < 175
		// the factor of 2 is a placeholderto move strength reduction further out should be replaced by racemodeTransitionFactor so that transition point of horizon type behaviour moves to match levelAngleLimit
            	const float inclinationLevelRatio = constrainf(((horizonCutoffDegrees-currentInclination)*2) / horizonCutoffDegrees, 0, 1);
          	horizonLevelStrength = inclinationLevelRatio;
        } else  { //horizon_tilt_effect>=175 means no leveling
         	horizonLevelStrength = 0; 		  
        }
    }
    return constrainf(horizonLevelStrength, 0, 1);
}


static float pidLevel(int axis, const pidProfile_t *pidProfile, const rollAndPitchTrims_t *angleTrim, float currentPidSetpoint) {
    // calculate error angle and limit the angle to the max inclination
    // rcDeflection is in range [-1.0, 1.0]
    float angle = pidProfile->levelAngleLimit * getRcDeflection(axis);
#ifdef USE_GPS
    angle += GPS_angle[axis];
#endif
    angle = constrainf(angle, -pidProfile->levelAngleLimit, pidProfile->levelAngleLimit);
    const float errorAngle = angle - ((attitude.raw[axis] - angleTrim->raw[axis]) / 10.0f);
	
    if (FLIGHT_MODE(ANGLE_MODE)) {
        // ANGLE mode - control is angle based
        currentPidSetpoint = errorAngle * levelGain;
    } else { // HORIZON hacked into 2 types of RACEMODE  - Expert Mode On is RACEMODEhoriozon or Off is RACEMODEangle  
        const float horizonLevelStrength = calcHorizonLevelStrength();
		const float racemodeInclination = MAX(ABS(attitude.values.roll), ABS(attitude.values.pitch)) / 10.0f;
		if (horizonTiltExpertMode) {//  horizon type racemode behaviour without a level limit - horizonTiltExpertMode is ON	
			currentPidSetpoint = currentPidSetpoint + (errorAngle * horizonGain * horizonLevelStrength);
		}else{  //angle limit type racemode behaviour  - horizonTiltExpertMode is OFF			
			// if current angle is less than max angle limit
			if (racemodeInclination < (pidProfile->levelAngleLimit)) {
			//  This should make roll stick behave like it does in angle mode constraining stick input to max angle just like angle mode
			currentPidSetpoint = errorAngle * horizonGain;	
			}else{
			//  modified horizon expert mode behaviour beyond max angle limit for roll axis that is only reachable by pitching to inverted or returning from inverted via roll axis		
			currentPidSetpoint = currentPidSetpoint + (errorAngle * horizonGain * horizonLevelStrength);		
			}		
		}
    }
    return currentPidSetpoint;
}

static float accelerationLimit(int axis, float currentPidSetpoint)
{
    static float previousSetpoint[3];
    const float currentVelocity = currentPidSetpoint- previousSetpoint[axis];

    if (ABS(currentVelocity) > maxVelocity[axis]) {
        currentPidSetpoint = (currentVelocity > 0) ? previousSetpoint[axis] + maxVelocity[axis] : previousSetpoint[axis] - maxVelocity[axis];
    }

    previousSetpoint[axis] = currentPidSetpoint;
    return currentPidSetpoint;
}

// Betaflight pid controller, which will be maintained in the future with additional features specialised for current (mini) multirotor usage.
// Based on 2DOF reference design (matlab)
void pidController(const pidProfile_t *pidProfile, const rollAndPitchTrims_t *angleTrim, timeUs_t currentTimeUs)
{
    static float previousRateError[2];
    const float tpaFactor = getThrottlePIDAttenuation();
    const float motorMixRange = getMotorMixRange();
    static timeUs_t crashDetectedAtUs;
    static timeUs_t previousTimeUs;

    // calculate actual deltaT in seconds
    const float deltaT = (currentTimeUs - previousTimeUs) * 0.000001f;
    previousTimeUs = currentTimeUs;

    // Dynamic i component,
    // gradually scale back integration when above windup point,
    // use dT (not deltaT) for ITerm calculation to avoid wind-up caused by jitter
    const float dynCi = MIN((1.0f - motorMixRange) * ITermWindupPointInv, 1.0f) * dT * itermAccelerator;

    // Dynamic d component, enable 2-DOF PID controller only for rate mode
    const float dynCd = flightModeFlags ? 0.0f : dtermSetpointWeight;

    // ----------PID controller----------
    for (int axis = FD_ROLL; axis <= FD_YAW; axis++) {
        float currentPidSetpoint = getSetpointRate(axis);
        if (maxVelocity[axis]) {
            currentPidSetpoint = accelerationLimit(axis, currentPidSetpoint);
        }
        // Yaw control is GYRO based, direct sticks control is applied to rate PID
	// Seperate horizon hacked to racemode and angle so ignoring pitch axis on racemode doesnt break angle mode
	if ((FLIGHT_MODE(HORIZON_MODE)) && ((axis != YAW)&&(axis !=PITCH))) {
            currentPidSetpoint = pidLevel(axis, pidProfile, angleTrim, currentPidSetpoint);
	}
	if ((FLIGHT_MODE(ANGLE_MODE)) && axis != YAW) {
            currentPidSetpoint = pidLevel(axis, pidProfile, angleTrim, currentPidSetpoint);
	}	
	    
        // -----calculate error rate
        const float gyroRate = gyro.gyroADCf[axis]; // Process variable from gyro output in deg/sec
        float errorRate = currentPidSetpoint - gyroRate; // r - y

        if (inCrashRecoveryMode && cmpTimeUs(currentTimeUs, crashDetectedAtUs) > crashTimeDelayUs) {
            if (pidProfile->crash_recovery == PID_CRASH_RECOVERY_BEEP) {
                BEEP_ON;
            }
            if (axis == FD_YAW) {
                errorRate = constrainf(errorRate, -crashLimitYaw, crashLimitYaw);
            } else {
                // on roll and pitch axes calculate currentPidSetpoint and errorRate to level the aircraft to recover from crash
                if (sensors(SENSOR_ACC)) {
                    // errorAngle is deviation from horizontal
                    const float errorAngle =  -(attitude.raw[axis] - angleTrim->raw[axis]) / 10.0f;
                    currentPidSetpoint = errorAngle * levelGain;
                    errorRate = currentPidSetpoint - gyroRate;
                }
            }
            // reset ITerm, since accumulated error before crash is now meaningless
            // and ITerm windup during crash recovery can be extreme, especially on yaw axis
            axisPID_I[axis] = 0.0f;
            if (cmpTimeUs(currentTimeUs, crashDetectedAtUs) > crashTimeLimitUs
                || (motorMixRange < 1.0f
                       && ABS(gyro.gyroADCf[FD_ROLL]) < crashRecoveryRate
                       && ABS(gyro.gyroADCf[FD_PITCH]) < crashRecoveryRate
                       && ABS(gyro.gyroADCf[FD_YAW]) < crashRecoveryRate)) {
                if (sensors(SENSOR_ACC)) {
                    // check aircraft nearly level
                    if (ABS(attitude.raw[FD_ROLL] - angleTrim->raw[FD_ROLL]) < crashRecoveryAngleDeciDegrees
                       && ABS(attitude.raw[FD_PITCH] - angleTrim->raw[FD_PITCH]) < crashRecoveryAngleDeciDegrees) {
                        inCrashRecoveryMode = false;
                        BEEP_OFF;
                    }
                } else {
                    inCrashRecoveryMode = false;
                    BEEP_OFF;
                }
            }
        }

        // --------low-level gyro-based PID based on 2DOF PID controller. ----------
        // 2-DOF PID controller with optional filter on derivative term.
        // b = 1 and only c (dtermSetpointWeight) can be tuned (amount derivative on measurement or error).

        // -----calculate P component and add Dynamic Part based on stick input
        axisPID_P[axis] = Kp[axis] * errorRate * tpaFactor;
        if (axis == FD_YAW) {
            axisPID_P[axis] = ptermYawFilterApplyFn(ptermYawFilter, axisPID_P[axis]);
        }

        // -----calculate I component
        const float ITerm = axisPID_I[axis];
        const float ITermNew = constrainf(ITerm + Ki[axis] * errorRate * dynCi, -itermLimit, itermLimit);
        const bool outputSaturated = mixerIsOutputSaturated(axis, errorRate);
        if (outputSaturated == false || ABS(ITermNew) < ABS(ITerm)) {
            // Only increase ITerm if output is not saturated
            axisPID_I[axis] = ITermNew;
        }

        // -----calculate D component
        if (axis != FD_YAW) {
            // apply filters
            float gyroRateFiltered = dtermNotchFilterApplyFn(dtermFilterNotch[axis], gyroRate);
            gyroRateFiltered = dtermLpfApplyFn(dtermFilterLpf[axis], gyroRateFiltered);

            const float rD = dynCd * MIN(getRcDeflectionAbs(axis) * relaxFactor, 1.0f) * currentPidSetpoint - gyroRateFiltered;    // cr - y
            // Divide rate change by deltaT to get differential (ie dr/dt)
            float delta = (rD - previousRateError[axis]) / deltaT;

            previousRateError[axis] = rD;

            // if crash recovery is on and accelerometer enabled and there is no gyro overflow, then check for a crash
            // no point in trying to recover if the crash is so severe that the gyro overflows
            if (pidProfile->crash_recovery && !gyroOverflowDetected()) {
                if (ARMING_FLAG(ARMED)) {
                    if (motorMixRange >= 1.0f && !inCrashRecoveryMode
                        && ABS(delta) > crashDtermThreshold
                        && ABS(errorRate) > crashGyroThreshold
                        && ABS(getSetpointRate(axis)) < crashSetpointThreshold) {
                        inCrashRecoveryMode = true;
                        crashDetectedAtUs = currentTimeUs;
                    }
                    if (inCrashRecoveryMode && cmpTimeUs(currentTimeUs, crashDetectedAtUs) < crashTimeDelayUs && (ABS(errorRate) < crashGyroThreshold
                        || ABS(getSetpointRate(axis)) > crashSetpointThreshold)) {
                        inCrashRecoveryMode = false;
                        BEEP_OFF;
                    }
                } else if (inCrashRecoveryMode) {
                    inCrashRecoveryMode = false;
                    BEEP_OFF;
                }
            }
            axisPID_D[axis] = Kd[axis] * delta * tpaFactor;
            axisPIDSum[axis] = axisPID_P[axis] + axisPID_I[axis] + axisPID_D[axis];
        } else {
            axisPIDSum[axis] = axisPID_P[axis] + axisPID_I[axis];
        }

        // Disable PID control if at zero throttle or if gyro overflow detected
        if (!pidStabilisationEnabled || gyroOverflowDetected()) {
            axisPID_P[axis] = 0;
            axisPID_I[axis] = 0;
            axisPID_D[axis] = 0;
            axisPIDSum[axis] = 0;
        }
    }
}

bool crashRecoveryModeActive(void)
{
    return inCrashRecoveryMode;
}
