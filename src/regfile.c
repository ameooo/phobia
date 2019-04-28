#include <stddef.h>

#include "hal/hal.h"

#include "libc.h"
#include "main.h"
#include "regfile.h"
#include "shell.h"
#include "tel.h"

#define REG_DEF(l, e, u, f, m, p, t)	{ #l #e "\0" u, f, m, (void *) &l, (void *) p, (void *) t}
#define REG_MAX				(sizeof(regfile) / sizeof(reg_t) - 1UL)

static int		null;

static void
reg_proc_pwm(const reg_t *reg, float *lval, const float *rval)
{
	if (lval != NULL) {

		*lval = reg->link->f;
	}
	else if (rval != NULL) {

		reg->link->f = *rval;
		hal_fence();

		ADC_irq_lock();
		PWM_set_configuration();

		pm.freq_hz = hal.PWM_frequency;
		pm.dT = 1.f / pm.freq_hz;
		pm.dc_resolution = hal.PWM_resolution;

		ADC_irq_unlock();
	}
}

static void
reg_proc_ppm(const reg_t *reg, int *lval, const int *rval)
{
	if (lval != NULL) {

		*lval = reg->link->i;
	}
	else if (rval != NULL) {

		reg->link->i = *rval;
		hal_fence();

		PPM_set_configuration();
	}
}

static void
reg_proc_fsm_state(const reg_t *reg, int *lval, const int *rval)
{
	if (lval != NULL) {

		*lval = reg->link->i;
	}
	else if (rval != NULL) {

		pm_fsm_req(&pm, *rval);
	}
}

static void
reg_proc_rpm(const reg_t *reg, float *lval, const float *rval)
{
	if (lval != NULL) {

		*lval = reg->link->f * 9.5492969f / pm.const_Zp;
	}
	else if (rval != NULL) {

		reg->link->f = (*rval) * 0.10471976f * pm.const_Zp;
	}
}

static void
reg_proc_rpm_pc(const reg_t *reg, float *lval, const float *rval)
{
	float			KPC = PM_EMAX(&pm) / 100.f;

	if (lval != NULL) {

		*lval = reg->link->f * pm.const_E / (KPC * pm.const_lpf_U);
	}
	else if (rval != NULL) {

		reg->link->f = (*rval) * KPC * pm.const_lpf_U / pm.const_E;
	}
}

static void
reg_proc_kv(const reg_t *reg, float *lval, const float *rval)
{
        if (lval != NULL) {

                *lval = 5.513289f / (reg->link->f * pm.const_Zp);
        }
        else if (rval != NULL) {

                reg->link->f = 5.513289f / ((*rval) * pm.const_Zp);
        }
}

static void
reg_proc_clamp(const reg_t *reg, float *lval, const float *rval)
{
	float			maximal, adjust;

	if (lval != NULL) {

		*lval = reg->link->f;
	}
	else if (rval != NULL) {

		if (*rval < 0.f) {

			maximal = ADC_RESOLUTION * 95E-2f / 2.f;

			adjust = (pm.adjust_IA[1] < pm.adjust_IB[1])
				? pm.adjust_IA[1] : pm.adjust_IB[1];
			adjust = (adjust > .5f && adjust < 1.5f) ? adjust : 1.f;

			reg->link->f = (float) (int) (maximal * hal.ADC_const.GA * adjust);
		}
		else {

			reg->link->f = *rval;
		}
	}
}

static void
reg_proc_DQ(const reg_t *reg, float *lval, const float *rval)
{
	float			*DQ = (void *) reg->link;
	float			angle, f_cosine, f_sine;
	int			revol;

        if (lval != NULL) {

		angle = m_atan2f(DQ[1], DQ[0]);
		*lval = angle + (float) pm.x_setpoint_revol * 2.f * M_PI_F;
        }
        else if (rval != NULL) {

		angle = (*rval);
		revol = (int) (angle / (2.f * M_PI_F));
                angle -= (float) (revol * 2.f * M_PI_F);

                if (angle < - M_PI_F) {

                        revol -= 1;
                        angle += 2.f * M_PI_F;
                }

                if (angle > M_PI_F) {

                        revol += 1;
                        angle -= 2.f * M_PI_F;
                }

		f_cosine = m_cosf(angle);
		f_sine   = m_sinf(angle);

		hal_fence();

		/* FIXME: Multi-word data may be corrupted.
		 * */

		DQ[0] = f_cosine;
		DQ[1] = f_sine;
		pm.x_setpoint_revol = revol;
        }
}

static void
reg_proc_DQ_mg(const reg_t *reg, float *lval, const float *rval)
{
	float			angle;

	if (lval != NULL) {

		reg_proc_DQ(reg, &angle, rval);

		*lval = angle * (180.f / M_PI_F) / pm.const_Zp;
	}
	else if (rval != NULL) {

		angle = (*rval) * (M_PI_F / 180.f) * pm.const_Zp;

		reg_proc_DQ(reg, lval, &angle);
	}
}

static void
reg_format_ns(const reg_t *reg)
{
	float			fns;

	fns = (float) (reg->link->i) * 1000000000.f
		/ (hal.PWM_frequency * (float) hal.PWM_resolution);

	printf("%i (%1f ns)", reg->link->i, &fns);
}

static void
reg_format_self_BM(const reg_t *reg)
{
	int		*BM = (void *) reg->link;

	printf("%2x %2x %2x %2x %2x %2x %2x", BM[0], BM[1], BM[2], BM[3], BM[4], BM[5], BM[6]);
}

static void
reg_format_self_RMS(const reg_t *reg)
{
	float		*RMS = (void *) reg->link;

	printf("%3f %3f (A)", &RMS[0], &RMS[1]);
}

#define TEXT_ITEM(t)	case t: printf("(%s)", PM_SFI(t)); break

static void
reg_format_enum(const reg_t *reg)
{
	int			n, val;

	n = (int) (reg - regfile);
	val = reg->link->i;

	printf("%i ", val);

	switch (n) {

		case ID_HAL_HALL_MODE:

			switch (val) {

				TEXT_ITEM(HALL_DISABLED);
				TEXT_ITEM(HALL_SENSOR);
				TEXT_ITEM(HALL_DRIVE_QEP);

				default: break;
			}
			break;

		case ID_HAL_PPM_MODE:

			switch (val) {

				TEXT_ITEM(PPM_DISABLED);
				TEXT_ITEM(PPM_PULSE_WIDTH);
				TEXT_ITEM(PPM_STEP_DIR);
				TEXT_ITEM(PPM_CONTROL_QEP);
				TEXT_ITEM(PPM_I2C_BUS);
				TEXT_ITEM(PPM_DATA_USART);

				default: break;
			}
			break;

		case ID_PM_FAIL_REASON:

			printf("(%s)", pm_strerror(pm.fail_reason));
			break;

		case ID_PM_CONFIG_ABC:

			switch (val) {

				TEXT_ITEM(PM_ABC_THREE_PHASE);
				TEXT_ITEM(PM_ABC_TWO_PHASE);

				default: break;
			}
			break;

		case ID_PM_CONFIG_LDQ:

			switch (val) {

				TEXT_ITEM(PM_LDQ_NON_SALIENT_POLE);
				TEXT_ITEM(PM_LDQ_SALIENT_POLE);
				TEXT_ITEM(PM_LDQ_SATURATION_SALIENCY);

				default: break;
			}
			break;

		case ID_PM_CONFIG_VM:
		case ID_PM_CONFIG_HALL:
		case ID_PM_CONFIG_HFI:

			switch (val) {

				TEXT_ITEM(PM_DISABLED);
				TEXT_ITEM(PM_ENABLED);

				default: break;
			}
			break;

		case ID_PM_CONFIG_LOOP:

			switch (val) {

				TEXT_ITEM(PM_LOOP_DRIVE_CURRENT);
				TEXT_ITEM(PM_LOOP_DRIVE_SPEED);
				TEXT_ITEM(PM_LOOP_DRIVE_SERVO);
				TEXT_ITEM(PM_LOOP_RECTIFIER_VOLTAGE);

				default: break;
			}
			break;

		case ID_PM_FSM_STATE:

			switch (val) {

				TEXT_ITEM(PM_STATE_IDLE);
				TEXT_ITEM(PM_STATE_ZERO_DRIFT);
				TEXT_ITEM(PM_STATE_SELF_TEST_POWER_STAGE);
				TEXT_ITEM(PM_STATE_SELF_TEST_SAMPLING_ACCURACY);
				TEXT_ITEM(PM_STATE_STANDARD_VOLTAGE);
				TEXT_ITEM(PM_STATE_STANDARD_CURRENT);
				TEXT_ITEM(PM_STATE_ADJUST_VOLTAGE);
				TEXT_ITEM(PM_STATE_ADJUST_CURRENT);
				TEXT_ITEM(PM_STATE_PROBE_CONST_R);
				TEXT_ITEM(PM_STATE_PROBE_CONST_L);
				TEXT_ITEM(PM_STATE_LU_INITIATE);
				TEXT_ITEM(PM_STATE_LU_SHUTDOWN);
				TEXT_ITEM(PM_STATE_PROBE_CONST_E);
				TEXT_ITEM(PM_STATE_PROBE_CONST_J);
				TEXT_ITEM(PM_STATE_HALT);

				default: break;
			}
			break;

		case ID_PM_LU_MODE:

			switch (val) {

				TEXT_ITEM(PM_LU_DISABLED);
				TEXT_ITEM(PM_LU_DETACHED);
				TEXT_ITEM(PM_LU_FORCED);
				TEXT_ITEM(PM_LU_ESTIMATE_FLUX);
				TEXT_ITEM(PM_LU_ESTIMATE_HFI);
				TEXT_ITEM(PM_LU_SENSORED_HALL);

				default: break;
			}
			break;

		default: break;
	}
}

const reg_t		regfile[] = {

	REG_DEF(null,,				"",	"%i",	REG_READ_ONLY, NULL, NULL),

	REG_DEF(hal.HSE_crystal_clock,,		"Hz",	"%i",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(hal.USART_baud_rate,,		"",	"%i",	REG_CONFIG, NULL, NULL),
	REG_DEF(hal.PWM_frequency,,		"Hz",	"%1f",	REG_CONFIG, &reg_proc_pwm, NULL),
	REG_DEF(hal.PWM_deadtime,,		"ns",	"%1f",	REG_CONFIG, &reg_proc_pwm, NULL),
	REG_DEF(hal.ADC_reference_voltage,,	"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(hal.ADC_shunt_resistance,,	"Ohm",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(hal.ADC_amplifier_gain,,	"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(hal.ADC_voltage_ratio,,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(hal.ADC_terminal_ratio,,	"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(hal.ADC_terminal_bias,,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(hal.HALL_mode,,		"",	"%i", REG_CONFIG, &reg_proc_ppm, &reg_format_enum),
	REG_DEF(hal.HALL_sensor_state,,		"",	"%i",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(hal.PPM_mode,,		"",	"%i", REG_CONFIG, &reg_proc_ppm, &reg_format_enum),
	REG_DEF(hal.PPM_timebase,,		"Hz",	"%i",	REG_CONFIG, NULL, NULL),
	REG_DEF(hal.PPM_signal_caught,,		"",	"%i",	REG_READ_ONLY, NULL, NULL),

	REG_DEF(ap.ppm_reg_ID,,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ap.ppm_pulse_range[0],,		"us",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ppm_pulse_range[1],,		"us",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ppm_control_range[0],,	"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ppm_control_range[1],,	"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ppm_safe_range[0],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ppm_safe_range[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),

	REG_DEF(ap.analog_reg_ID,,		"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ap.analog_timeout,,		"s",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.analog_voltage_range[0],,	"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.analog_voltage_range[1],,	"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.analog_control_range[0],,	"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.analog_control_range[1],,	"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.analog_safe_range[0],,	"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.analog_safe_range[1],,	"",	"%4e",	REG_CONFIG, NULL, NULL),

	REG_DEF(ap.ntc_PCB.r_balance,,		"Ohm",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ntc_PCB.r_ntc_0,,		"Ohm",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ntc_PCB.ta_0,,		"C",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ntc_PCB.betta,,		"",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ntc_EXT.r_balance,,		"Ohm",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ntc_EXT.r_ntc_0,,		"Ohm",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ntc_EXT.ta_0,,		"C",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.ntc_EXT.betta,,		"",	"%1f",	REG_CONFIG, NULL, NULL),

	REG_DEF(ap.temp_PCB,,			"C",	"%1f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(ap.temp_EXT,,			"C",	"%1f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(ap.temp_INT,,			"C",	"%1f",	REG_READ_ONLY, NULL, NULL),

	REG_DEF(ap.heat_PCB,,			"C",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.heat_PCB_derated,,		"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.heat_EXT,,			"C",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.heat_EXT_derated,,		"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.heat_PCB_FAN,,		"C",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.heat_hysteresis,,		"C",	"%1f",	REG_CONFIG, NULL, NULL),

	REG_DEF(ap.batt_voltage_low,,		"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.batt_hysteresis,,		"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.batt_derated,,		"W",	"%1f",	REG_CONFIG, NULL, NULL),

	REG_DEF(ap.pull_g,,			"g",	"%1f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(ap.pull_adjust[0],,		"g",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(ap.pull_adjust[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.dc_resolution,,	"",	"%i",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.dc_minimal,,		"",	"%i",	REG_CONFIG, NULL, &reg_format_ns),
	REG_DEF(pm.dc_clearance,,	"",	"%i",	REG_CONFIG, NULL, &reg_format_ns),

	REG_DEF(pm.fail_reason,,	"",	"%i",	REG_READ_ONLY, NULL, &reg_format_enum),
	REG_DEF(pm.self_BM,,		"",	"%i",	REG_READ_ONLY, NULL, &reg_format_self_BM),
	REG_DEF(pm.self_RMS,,		"",	"%i",	REG_READ_ONLY, NULL, &reg_format_self_RMS),

	REG_DEF(pm.config_ABC,,		"",	"%i",	REG_CONFIG, NULL, &reg_format_enum),
	REG_DEF(pm.config_LDQ,,		"",	"%i",	REG_CONFIG, NULL, &reg_format_enum),
	REG_DEF(pm.config_VM,,		"",	"%i",	REG_CONFIG, NULL, &reg_format_enum),
	REG_DEF(pm.config_HALL,,	"",	"%i",	REG_CONFIG, NULL, &reg_format_enum),
	REG_DEF(pm.config_HFI,,		"",	"%i",	REG_CONFIG, NULL, &reg_format_enum),
	REG_DEF(pm.config_LOOP,,	"",	"%i",	REG_CONFIG, NULL, &reg_format_enum),

	REG_DEF(pm.fsm_state,,		"",	"%i",	0, &reg_proc_fsm_state, &reg_format_enum),
	REG_DEF(pm.fsm_phase,,			"",	"%i",	0, NULL, NULL),

	REG_DEF(pm.tm_transient_slow,, 		"s",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.tm_transient_fast,, 		"s",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.tm_voltage_hold,, 		"s",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.tm_current_hold,, 		"s",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.tm_instant_probe,, 		"s",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.tm_average_drift,, 		"s",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.tm_average_probe,, 		"s",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.tm_startup,,			"s",	"%3f",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.adjust_IA[0],,		"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_IA[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_IB[0],,		"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_IB[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_US[0],,		"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_US[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_UA[0],,		"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_UA[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_UB[0],,		"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_UB[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_UC[0],,		"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.adjust_UC[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.fb_current_clamp,,		"A",	"%3f",	REG_CONFIG, &reg_proc_clamp, NULL),
	REG_DEF(pm.fb_current_A,,		"A",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.fb_current_B,,		"A",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.fb_voltage_A,,		"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.fb_voltage_B,,		"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.fb_voltage_C,,		"V",	"%3f",	REG_READ_ONLY, NULL, NULL),

	REG_DEF(pm.probe_current_hold_X,,	"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.probe_current_hold_Y,,	"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.probe_current_sine,,		"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.probe_freq_sine_hz,,		"Hz",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.probe_speed_low,,		"rad/s","%2f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.probe_speed_low, _rpm,	"rpm",	"%2f",	0, &reg_proc_rpm, NULL),
	REG_DEF(pm.probe_speed_ramp,,		"rad/s","%2f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.probe_speed_ramp, _rpm,	"rpm",	"%2f",	0, &reg_proc_rpm, NULL),
	REG_DEF(pm.probe_gain_P,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.probe_gain_I,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.probe_impedance_R,,		"Ohm",	"%4e",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.probe_rotation_DQ,,		"g",	"%2f",	REG_READ_ONLY, NULL, NULL),

	REG_DEF(pm.fault_voltage_tolerance,,	"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.fault_current_tolerance,,	"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.fault_voltage_halt_level,,	"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.fault_current_halt_level,,	"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.fault_adjust_tolerance,,	"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.fault_flux_residue_maximal,,"A",	"%2f",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.vsi_precise_MODE,,		"",	"%i",	0, NULL, NULL),
	REG_DEF(pm.vsi_clamp_to_GND,,		"",	"%i",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vsi_current_ZONE,,		"",	"%i",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vsi_voltage_ZONE,,		"",	"%i",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vsi_X,,			"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vsi_Y,,			"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vsi_lpf_D,,			"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vsi_lpf_Q,,			"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vsi_lpf_watt,,		"W",	"%1f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vsi_gain_LP,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vsi_gain_LW,,		"",	"%2e",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.vm_maximal,,			"",	"%2f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_A,,			"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vm_B,,			"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vm_C,,			"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vm_FIR_A[0],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_FIR_A[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_FIR_A[2],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_FIR_B[0],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_FIR_B[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_FIR_B[2],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_FIR_C[0],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_FIR_C[1],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_FIR_C[2],,		"",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.vm_residue_X,,		"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.vm_residue_Y,,		"V",	"%3f",	REG_READ_ONLY, NULL, NULL),

	REG_DEF(pm.lu_X[0],,			"A",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.lu_X[1],,			"A",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.lu_X[2],,			"",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.lu_X[3],,			"",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.lu_X[4],,		"rad/s",	"%2f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.lu_X[4], _rpm,		"rpm",	"%2f",	REG_READ_ONLY, &reg_proc_rpm, NULL),
	REG_DEF(pm.lu_mode,,			"",	"%i",	REG_READ_ONLY, NULL, &reg_format_enum),

	REG_DEF(pm.forced_hold_D,,		"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.forced_maximal,,		"rad",	"%2f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.forced_maximal, _rpm,	"rpm",	"%2f",	0, &reg_proc_rpm, NULL),
	REG_DEF(pm.forced_accel,,	"rad/s/s",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.forced_accel, _rpm,	"rpm/s",	"%1f",	0, &reg_proc_rpm, NULL),

	REG_DEF(pm.flux_X[0],,			"A",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.flux_X[1],,			"A",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.flux_X[2],,			"",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.flux_X[3],,			"",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.flux_X[4],,		"rad/s",	"%2f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.flux_X[4], _rpm,		"rpm",	"%2f",	REG_READ_ONLY, &reg_proc_rpm, NULL),
	REG_DEF(pm.flux_drift_Q,,		"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.flux_residue_D,,		"A",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.flux_residue_Q,,		"A",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.flux_residue_lpf,,		"",	"%2f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.flux_gain_LP,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.flux_gain_DA,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.flux_gain_QA,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.flux_gain_DP,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.flux_gain_DS,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.flux_gain_QS,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.flux_gain_QZ,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.flux_bemf_unlock,,		"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.flux_bemf_lock,,		"V",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.flux_bemf_high,,		"V",	"%3f",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.hfi_freq_hz,,		"Hz",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.hfi_swing_D,,		"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.hfi_flux,,			"",	"%4e",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.hfi_gain_P,,			"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.hfi_gain_S,,			"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.hfi_gain_F,,			"",	"%2e",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.const_lpf_U,,		"V",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.const_gain_LP,,		"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.const_E,,			"Wb",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.const_E, _kv,	"rpm/v",	"%1f",	0, &reg_proc_kv, NULL),
	REG_DEF(pm.const_R,,			"Ohm",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.const_Ld,,			"H",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.const_Lq,,			"H",	"%4e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.const_Zp,,			"",	"%i",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.const_J,,		"kg*m*m",	"%4e",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.i_maximal,,			"A",	"%3f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.i_derated,,			"A",	"%3f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.i_watt_maximal,,		"W",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.i_watt_reverse,,		"W",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.i_watt_derated,,		"W",	"%1f",	REG_READ_ONLY, NULL, NULL),
	REG_DEF(pm.i_setpoint_D,,		"A",	"%3f",	0, NULL, NULL),
	REG_DEF(pm.i_setpoint_Q,,		"A",	"%3f",	0, NULL, NULL),
	REG_DEF(pm.i_gain_PD,,			"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.i_gain_ID,,			"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.i_gain_PQ,,			"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.i_gain_IQ,,			"",	"%2e",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.s_maximal,,		"rad/s",	"%2f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.s_maximal, _rpm,		"rpm",	"%2f",	0, &reg_proc_rpm, NULL),
	REG_DEF(pm.s_setpoint,,		"rad/s",	"%2f",	0, NULL, NULL),
	REG_DEF(pm.s_setpoint, _rpm,		"rpm",	"%2f",	0, &reg_proc_rpm, NULL),
	REG_DEF(pm.s_setpoint, _pc,		"%",	"%2f",	0, &reg_proc_rpm_pc, NULL),
	REG_DEF(pm.s_accel,,		"rad/s/s",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.s_accel, _rpm,	"rpm/s",	"%1f",	0, &reg_proc_rpm, NULL),
	REG_DEF(pm.s_gain_P,,			"",	"%2e",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.s_gain_I,,			"",	"%2e",	REG_CONFIG, NULL, NULL),

	REG_DEF(pm.x_setpoint_DQ,,		"rad",	"%2f",	0, &reg_proc_DQ, NULL),
	REG_DEF(pm.x_setpoint_DQ, _mg,		"mg",	"%2f",	0, &reg_proc_DQ_mg, NULL),
	REG_DEF(pm.x_near_distance,,		"rad",	"%2f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.x_gain_P,,			"",	"%1f",	REG_CONFIG, NULL, NULL),
	REG_DEF(pm.x_gain_near_P,,		"",	"%1f",	REG_CONFIG, NULL, NULL),

	REG_DEF(ti.reg_ID[0],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ti.reg_ID[1],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ti.reg_ID[2],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ti.reg_ID[3],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ti.reg_ID[4],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ti.reg_ID[5],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ti.reg_ID[6],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ti.reg_ID[7],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ti.reg_ID[8],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),
	REG_DEF(ti.reg_ID[9],,			"",	"%i",	REG_CONFIG | REG_LINKED, NULL, NULL),

	{ NULL, "", 0, NULL, NULL, NULL }
};

void reg_getval(const reg_t *reg, void *lval)
{
	if (reg->proc != NULL) {

		reg->proc(reg, lval, NULL);
	}
	else {
		*(reg_val_t *) lval = *reg->link;
	}
}

void reg_setval(const reg_t *reg, const void *rval)
{
	if ((reg->mode & REG_READ_ONLY) == 0) {

		if (reg->proc != NULL) {

			reg->proc(reg, NULL, rval);
		}
		else {
			*reg->link = *(reg_val_t *) rval;
		}
	}
}

void reg_format_rval(const reg_t *reg, const void *rval)
{
	reg_val_t		*link = (reg_val_t *) rval;

	if (reg->fmt[1] == 'i') {

		printf(reg->fmt, link->i);
	}
	else {
		printf(reg->fmt, &link->f);
	}
}

void reg_format(const reg_t *reg)
{
	reg_val_t		rval;
	const char		*su;

	if (reg != NULL) {

		printf("%c%c%c [%i] %s = ",
			(int) (reg->mode & REG_CONFIG) 		? 'C' : ' ',
			(int) (reg->mode & REG_READ_ONLY)	? 'R' : ' ',
			(int) (reg->mode & REG_LINKED)		? 'L' : ' ',
			(int) (reg - regfile), reg->sym);

		if (reg->format != NULL) {

			reg->format(reg);
		}
		else {
			reg_getval(reg, &rval);
			reg_format_rval(reg, &rval);

			if (reg->mode & REG_LINKED) {

				if (rval.i >= 0 && rval.i < REG_MAX) {

					printf(" (%s)", regfile[rval.i].sym);
				}
			}

			su = reg->sym + strlen(reg->sym) + 1;

			if (*su != 0) {

				printf(" (%s)", su);
			}
		}

		puts(EOL);
	}
}

const reg_t *reg_search(const char *sym)
{
	const reg_t		*reg, *found = NULL;
	int			n;

	if (stoi(&n, sym) != NULL) {

		if (n >= 0 && n < REG_MAX)
			found = regfile + n;
	}
	else {
		for (reg = regfile; reg->sym != NULL; ++reg) {

			if (strstr(reg->sym, sym) != NULL) {

				if (found == NULL) {

					found = reg;
				}
				else {
					found = NULL;
					break;
				}
			}
		}
	}

	return found;
}

void reg_GET(int n, void *lval)
{
	if (n >= 0 && n < REG_MAX) {

		reg_getval(regfile + n, lval);
	}
}

void reg_SET(int n, const void *rval)
{
	if (n >= 0 && n < REG_MAX) {

		reg_setval(regfile + n, rval);
	}
}

void reg_SET_I(int n, int rval)
{
	reg_SET(n, &rval);
}

void reg_SET_F(int n, float rval)
{
	reg_SET(n, &rval);
}

SH_DEF(reg)
{
	reg_val_t		rval;
	const reg_t		*reg, *lreg;

	reg = reg_search(s);

	if (reg != NULL) {

		s = sh_next_arg(s);

		if (reg->fmt[1] == 'i') {

			if (reg->mode & REG_LINKED) {

				lreg = reg_search(s);

				if (lreg != NULL) {

					rval.i = (int) (lreg - regfile);
					reg_setval(reg, &rval);
				}
			}
			else if (stoi(&rval.i, s) != NULL) {

				reg_setval(reg, &rval);
			}
		}
		else {
			if (stof(&rval.f, s) != NULL) {

				reg_setval(reg, &rval);
			}
		}

		reg_format(reg);
	}
	else {
		for (reg = regfile; reg->sym != NULL; ++reg) {

			if (strstr(reg->sym, s) != NULL) {

				reg_format(reg);
			}
		}
	}
}

SH_DEF(reg_export)
{
	reg_val_t		rval;
	const reg_t		*reg;

	for (reg = regfile; reg->sym != NULL; ++reg) {

		if (reg->mode & REG_CONFIG) {

			printf("reg %s ", reg->sym);

			reg_getval(reg, &rval);

			if (reg->mode & REG_LINKED) {

				if (rval.i >= 0 && rval.i < REG_MAX) {

					puts(regfile[rval.i].sym);
				}
			}
			else {
				reg_format_rval(reg, &rval);
			}

			puts(EOL);
		}
	}
}

