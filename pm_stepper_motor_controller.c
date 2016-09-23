//###########################################################################
// FILE:   pm_stepper_motor_controller.c
// TITLE:  Adaptive controller for a Permanent Magnet Stepper Motor
//###########################################################################
// $TI Release: F2837xS Support Library v190 $
// $Release Date: Mon Feb  1 16:59:09 CST 2016 $
// $Copyright: Copyright (C) 2014-2016 Texas Instruments Incorporated -
//             http://www.ti.com/ ALL RIGHTS RESERVED $
//###########################################################################

#include "F28x_Project.h"     // Device Headerfile and Examples Include File
#include <math.h>

void SelectGPIO(void);
void ConfigureADC(void);
void ConfigureEPWM(void);
void ConfigureEPWM7(void);
void ConfigureEPWM9(void);
void ConfigureEQEP1(void);
void SetupADCEpwm(Uint16 channel);
void SetPWMA(float);
void SetPWMB(float);
__interrupt void adca1_isr(void);
__interrupt void adcb1_isr(void);
__interrupt void cpu_timer0_isr(void);
//////////////////////////////////////////////////						//////////////////////////////////////////////////
//////////////////////////////////////////////////   Controller Gains	//////////////////////////////////////////////////
//////////////////////////////////////////////////						//////////////////////////////////////////////////
#define Kp 0.07
#define Kd 0.00001
#define AlphaA 2
#define AlphaB 2
#define Gamma2 1
#define Gamma5 1
#define GammakP 1
#define GammakA 1
#define gamma 1
#define N 3
//////////////////////////////////////////////////						//////////////////////////////////////////////////
//////////////////////////////////////////////////   System Constants	//////////////////////////////////////////////////
//////////////////////////////////////////////////						//////////////////////////////////////////////////
#define R 5.0					// Phase Winding Resistance (Ohm)
#define L 0.006				// Phase Winding Inductance (H)
#define km 0.15					// Motor Torque Constant (N*m/A)
#define J 0.0001872				// Rotor Inertia (kg*m^2)
#define b 0.002					// Rotor Damping (N*m/(rad/s))
#define Nr 100					// Number of teeth
#define Ts 0.001
#define iTs 1/Ts
#define Vmax 12
#define tf 10
#define JkmC 1/(J*km)
#define kmI 1/km
#define VmaxI 1/(Vmax+0.5)
//////////////////////////////////////////////////						//////////////////////////////////////////////////
//////////////////////////////////////////////////     uC Constants	    //////////////////////////////////////////////////
//////////////////////////////////////////////////						//////////////////////////////////////////////////
#define EPWM7_TIMER_TBPRD  5000	// Period Register 10kHz
#define EPWM7_CMPA     5000	    // 0 = 100% Duty Cycle; TBPRD = 0% Duty Cycle
#define EPWM7_DB   0x007F		// PWM Dead Band
#define EPWM9_TIMER_TBPRD  5000 // Period Register 10kHz
#define EPWM9_CMPA     5000		// 0 = 100% Duty Cycle; TBPRD = 0% Duty Cycle
#define EPWM9_DB   0x007F		// PWM Dead Band
#define RESULTS_BUFFER_SIZE 5000
//////////////////////////////////////////////////						//////////////////////////////////////////////////
//////////////////////////////////////////////////  System Variables    //////////////////////////////////////////////////
//////////////////////////////////////////////////						//////////////////////////////////////////////////
#pragma DATA_SECTION(ThetaArray, "SVArray")
#pragma DATA_SECTION(DThetaArray, "SVArray")
#pragma DATA_SECTION(IaArray, "SVArray")
#pragma DATA_SECTION(IbArray, "SVArray")
#pragma DATA_SECTION(VaArray, "SVArray")
#pragma DATA_SECTION(VbArray, "SVArray")
float ThetaArray[RESULTS_BUFFER_SIZE], DThetaArray[RESULTS_BUFFER_SIZE], IaArray[RESULTS_BUFFER_SIZE];
float IbArray[RESULTS_BUFFER_SIZE], VaArray[RESULTS_BUFFER_SIZE], VbArray[RESULTS_BUFFER_SIZE];
float time, Va, Vb, Theta, ThetaD, DTheta, DThetaD, DDThetaD, DDDThetaD, Ia, Ib, IaD, IbD, Tau;
int index = 0, load = 0;
// static float  gammakP[N], gammakA[N], Sigma2, Sigma5;

void main(void){
	// Initialize System Control: PLL, WatchDog, enable Peripheral Clocks.
	// This example function is found in the F2837xS_SysCtrl.c file.
    InitSysCtrl();
    // Initialize GPIO:This example function is found in the F2837xS_Gpio.c file
    InitGpio();
    SelectGPIO();
    // Enable PWM7 and PWM9
    CpuSysRegs.PCLKCR2.bit.EPWM7 = 1;
    CpuSysRegs.PCLKCR2.bit.EPWM9 = 1;
    // Clear all interrupts and initialize PIE vector table: Disable CPU interrupts
    DINT;
    // Initialize the PIE control registers to their default state. The default state is all PIE interrupts disabled
    // and flags are cleared. This function is found in the F2837xS_PieCtrl.c file.
    InitPieCtrl();
    // Disable CPU interrupts and clear all CPU interrupt flags:
    IER = 0x0000;
    IFR = 0x0000;
    // Initialize the PIE vector table with pointers to the shell Interrupt Service Routines (ISR).
    // This will populate the entire table, even if the interrupt is not used in this example.
    // The shell ISR routines are found in F2837xS_DefaultIsr.c. This function is found in F2837xS_PieVect.c.
    InitPieVectTable();
    // Map ISR functions
    EALLOW;
    PieVectTable.ADCA1_INT = &adca1_isr; // Function for ADCA interrupt 1
    PieVectTable.ADCB1_INT = &adcb1_isr; // Function for ADCB interrupt 1
    PieVectTable.TIMER0_INT = &cpu_timer0_isr; // Function for Timer0 Interrupt
    EDIS;
    InitCpuTimers(); // Basic setup CPU Timer0, 1 and 2
    ConfigCpuTimer(&CpuTimer0, 200, iTs); // CPU - Timer0 at 1 milisecond
    StopCpuTimer0();
    ConfigureADC();
    ConfigureEPWM();
    ConfigureEPWM7();
    ConfigureEPWM9();
    ConfigureEQEP1();
    SetupADCEpwm(0);// Setup the ADC for ePWM triggered conversions on channel 0
    // Enable global Interrupts and higher priority real-time debug events:
    IER |= M_INT1; // Enable group 1 interrupts
    EINT;  // Enable Global interrupt INTM
    //ERTM;  // Enable Global realtime interrupt DBGM
    //Initialize results buffer
	for(index = 0; index < RESULTS_BUFFER_SIZE; index++)
	{
		ThetaArray[index] = 0;
		DThetaArray[index] = 0;
		IaArray[index] = 0;
		IbArray[index] = 0;
		VaArray[index] = 0;
		VbArray[index] = 0;
	}
	index = 0;
	// Enable PIE interrupt
	PieCtrlRegs.PIEIER1.bit.INTx1 = 1; // ADC A Interrupt
	PieCtrlRegs.PIEIER1.bit.INTx2 = 1; // ADC B Interrupt
	PieCtrlRegs.PIEIER1.bit.INTx7 = 1; // Timer 0 Interrupt
	// Sync ePWM
    EALLOW;
    CpuSysRegs.PCLKCR0.bit.TBCLKSYNC = 1;
    StartCpuTimer0(); // CpuTimer0Regs.TCR.bit.TSS = 0; // Start timer0
    // Start ePWM
    EPwm1Regs.ETSEL.bit.SOCAEN = 1;  // Enable SOCA
	EPwm1Regs.TBCTL.bit.CTRMODE = 0; // Unfreeze, and enter up count mode
	EDIS;
    do{
    	asm(" NOP");
    }while(1);
}
void SelectGPIO(void){
	GPIO_SetupPinMux(10, GPIO_MUX_CPU1, 5); //EQEP1A
	GPIO_SetupPinOptions(10, GPIO_INPUT, GPIO_SYNC);
	GPIO_SetupPinMux(11, GPIO_MUX_CPU1, 5); //EQEP1B
	GPIO_SetupPinOptions(11, GPIO_INPUT, GPIO_SYNC);
	GPIO_SetupPinMux(13, GPIO_MUX_CPU1, 0); //LED
	GPIO_SetupPinOptions(13, GPIO_OUTPUT, GPIO_ASYNC);
	GPIO_SetupPinMux(12, GPIO_MUX_CPU1, 1); //PWM7A
	GPIO_SetupPinOptions(12, GPIO_OUTPUT, GPIO_ASYNC);
	GPIO_SetupPinMux(15, GPIO_MUX_CPU1, 0); //GPIO15-DirectionA
	GPIO_SetupPinOptions(15, GPIO_OUTPUT, GPIO_ASYNC);
	GPIO_SetupPinMux(16, GPIO_MUX_CPU1, 5); //PWM9A
	GPIO_SetupPinOptions(16, GPIO_OUTPUT, GPIO_ASYNC);
	GPIO_SetupPinMux(17, GPIO_MUX_CPU1, 0); //GPIO17-DirectionB
	GPIO_SetupPinOptions(17, GPIO_OUTPUT, GPIO_ASYNC);
	//GpioDataRegs.GPATOGGLE.bit.GPIO13 = 1;
	GpioDataRegs.GPACLEAR.bit.GPIO13 = 1;
}
//Write ADC configurations and power up the ADC for both ADC A and ADC B
void ConfigureADC(){
	EALLOW;
	//write configurations
	AdcaRegs.ADCCTL2.bit.PRESCALE = 6; //set ADCCLK divider to /4
	AdcbRegs.ADCCTL2.bit.PRESCALE = 6; //set ADCCLK divider to /4
    AdcSetMode(ADC_ADCA, ADC_RESOLUTION_12BIT, ADC_SIGNALMODE_SINGLE);
    AdcSetMode(ADC_ADCB, ADC_RESOLUTION_12BIT, ADC_SIGNALMODE_SINGLE);
	//Set pulse positions to late
	AdcaRegs.ADCCTL1.bit.INTPULSEPOS = 1;
	AdcbRegs.ADCCTL1.bit.INTPULSEPOS = 1;
	//power up the ADC
	AdcaRegs.ADCCTL1.bit.ADCPWDNZ = 1;
	AdcbRegs.ADCCTL1.bit.ADCPWDNZ = 1;
	//delay for 1ms to allow ADC time to power up
	DELAY_US(1000);
	EDIS;
}

void ConfigureEPWM(){
	EALLOW;
	// Assumes ePWM clock is already enabled
	EPwm1Regs.ETSEL.bit.SOCAEN	= 0;	        // Disable SOC on A group
	EPwm1Regs.ETSEL.bit.SOCASEL	= 4;	        // Select SOC on up-count
	EPwm1Regs.ETPS.bit.SOCAPRD = 1;		        // Generate pulse on 1st event
	EPwm1Regs.CMPA.bit.CMPA = 0x6096;           // Set compare A value to 2048 counts 0x0800
	EPwm1Regs.TBPRD = 0xC12C;			        // Set period to 4096 counts 0x1000
	EPwm1Regs.TBCTL.bit.CTRMODE = 3;            // freeze counter
	EDIS;
}

void ConfigureEPWM7(){
	EALLOW;
	EPwm7Regs.TBPRD = EPWM7_TIMER_TBPRD;           // Set timer period
	EPwm7Regs.TBPHS.bit.TBPHS = 0x0000;            // Phase is 0
	EPwm7Regs.TBCTR = 0x0000;                      // Clear counter
	EPwm7Regs.TBCTL.bit.CTRMODE = TB_COUNT_UPDOWN; // Count up
	EPwm7Regs.TBCTL.bit.PHSEN = TB_DISABLE;        // Disable phase loading
	EPwm7Regs.TBCTL.bit.HSPCLKDIV = TB_DIV1;       // Clock ratio to SYSCLKOUT
	EPwm7Regs.TBCTL.bit.CLKDIV = TB_DIV1;          // Slow so we can observe on the scope
	EPwm7Regs.CMPA.bit.CMPA = EPWM7_CMPA;
	EPwm7Regs.AQCTLA.bit.CAU = AQ_SET;             // Set PWM3A on Zero
	EPwm7Regs.AQCTLA.bit.CAD = AQ_CLEAR;
	// Active high complementary PWMs - Setup the deadband
	EPwm7Regs.DBCTL.bit.OUT_MODE = DB_FULL_ENABLE;
	EPwm7Regs.DBCTL.bit.POLSEL = DB_ACTV_HIC;
	EPwm7Regs.DBCTL.bit.IN_MODE = DBA_ALL;
	EPwm7Regs.DBRED.bit.DBRED = EPWM7_DB;
	EPwm7Regs.DBFED.bit.DBFED = EPWM7_DB;
	EDIS;
}

void ConfigureEPWM9(){
	EALLOW;
    EPwm9Regs.TBPRD = EPWM9_TIMER_TBPRD;           // Set timer period
    EPwm9Regs.TBPHS.bit.TBPHS = 0x0000;            // Phase is 0
    EPwm9Regs.TBCTR = 0x0000;                      // Clear counter
    EPwm9Regs.TBCTL.bit.CTRMODE = TB_COUNT_UPDOWN; // Count up
    EPwm9Regs.TBCTL.bit.PHSEN = TB_DISABLE;        // Disable phase loading
    EPwm9Regs.TBCTL.bit.HSPCLKDIV = TB_DIV1;       // Clock ratio to SYSCLKOUT
    EPwm9Regs.TBCTL.bit.CLKDIV = TB_DIV1;          // Slow so we can observe on the scope
    EPwm9Regs.CMPA.bit.CMPA = EPWM9_CMPA;
    EPwm9Regs.AQCTLA.bit.CAU = AQ_SET;             // Set PWM3A on Zero
    EPwm9Regs.AQCTLA.bit.CAD = AQ_CLEAR;
    // Active high complementary PWMs - Setup the deadband
    EPwm9Regs.DBCTL.bit.OUT_MODE = DB_FULL_ENABLE;
    EPwm9Regs.DBCTL.bit.POLSEL = DB_ACTV_HIC;
    EPwm9Regs.DBCTL.bit.IN_MODE = DBA_ALL;
    EPwm9Regs.DBRED.bit.DBRED = EPWM9_DB;
    EPwm9Regs.DBFED.bit.DBFED = EPWM9_DB;
    EDIS;
}

void ConfigureEQEP1(){
	EALLOW;
    EQep1Regs.QUPRD=2000000;         // Unit Timer for 100Hz at 200 MHz SYSCLKOUT
    EQep1Regs.QDECCTL.bit.QSRC=00;      // QEP quadrature count mode
    EQep1Regs.QEPCTL.bit.FREE_SOFT=2;
    EQep1Regs.QEPCTL.bit.PCRM=00;       // PCRM=00 mode - QPOSCNT reset on index event
    EQep1Regs.QEPCTL.bit.UTE=1;         // Unit Timeout Enable
    EQep1Regs.QEPCTL.bit.QCLM=1;        // Latch on unit time out
    EQep1Regs.QPOSMAX=0xffffffff;
    EQep1Regs.QEPCTL.bit.QPEN=1;        // QEP enable
    EQep1Regs.QCAPCTL.bit.UPPS=0;       // 1/32 for unit position  5
    EQep1Regs.QCAPCTL.bit.CCPS=0;       // 1/64 for CAP clock   6
    EQep1Regs.QCAPCTL.bit.CEN=1;        // QEP Capture Enable
    EDIS;
}

void SetupADCEpwm(Uint16 channel){
	Uint16 acqps;
	//determine minimum acquisition window (in SYSCLKS) based on resolution
	if(ADC_RESOLUTION_12BIT == AdcaRegs.ADCCTL2.bit.RESOLUTION){
		acqps = 14; //75ns
	}
	else { //resolution is 16-bit
		acqps = 63; //320ns
	}
	//Select the channels to convert and end of conversion flag
	EALLOW;
	AdcaRegs.ADCSOC0CTL.bit.CHSEL = channel;  //SOC0 will convert pin A0
	AdcaRegs.ADCSOC0CTL.bit.ACQPS = acqps; //sample window is 100 SYSCLK cycles
	AdcbRegs.ADCSOC0CTL.bit.CHSEL = channel;  //SOC0 will convert pin B0
	AdcbRegs.ADCSOC0CTL.bit.ACQPS = acqps; //sample window is acqps + 1 SYSCLK cycles
	AdcaRegs.ADCSOC0CTL.bit.TRIGSEL = 5; //trigger on ePWM1 SOCA/C. 01h ADCTRIG1 - CPU1 Timer 0, TINT0n
	AdcbRegs.ADCSOC0CTL.bit.TRIGSEL = 5; //trigger on ePWM1 SOCA/C
	AdcaRegs.ADCINTSEL1N2.bit.INT1SEL = 0; //end of SOC0 will set INT1 flag
	AdcaRegs.ADCINTSEL1N2.bit.INT1E = 1;   //enable INT1 flag
	AdcaRegs.ADCINTFLGCLR.bit.ADCINT1 = 1; //make sure INT1 flag is cleared
	AdcbRegs.ADCINTSEL1N2.bit.INT1SEL = 0; //end of SOC0 will set INT1 flag
	AdcbRegs.ADCINTSEL1N2.bit.INT1E = 1;   //enable INT1 flag
	AdcbRegs.ADCINTFLGCLR.bit.ADCINT1 = 1; //make sure INT1 flag is cleared
	EDIS;
}

void SetPWMA(float V){
	if (V>=0){GpioDataRegs.GPASET.bit.GPIO15 = 1;}
	else{GpioDataRegs.GPACLEAR.bit.GPIO15 = 1;}
	V = abs(V);
	if (V>Vmax) {V=Vmax;}
	V = V*EPwm7Regs.TBPRD*VmaxI;
	EPwm7Regs.CMPA.bit.CMPA = EPwm7Regs.TBPRD-V;
}

void SetPWMB(float V){
	if (V>=0){GpioDataRegs.GPASET.bit.GPIO17 = 1;}
	else{GpioDataRegs.GPACLEAR.bit.GPIO17 = 1;}
	V = abs(V);
	if (V>Vmax) {V=Vmax;}
	V = V*EPwm9Regs.TBPRD*VmaxI;
	EPwm9Regs.CMPA.bit.CMPA = EPwm9Regs.TBPRD-V;
}

__interrupt void adca1_isr(void){
	Ia = AdcaResultRegs.ADCRESULT0*0.000791452315L; //0.002137 R=1k
	AdcaRegs.ADCINTFLGCLR.bit.ADCINT1 = 1; //clear INT1 flag
	PieCtrlRegs.PIEACK.all = PIEACK_GROUP1;
}

__interrupt void adcb1_isr(void){
	Ib = AdcbResultRegs.ADCRESULT0*0.000791452315L;
	AdcbRegs.ADCINTFLGCLR.bit.ADCINT1 = 1; //clear INT1 flag
	PieCtrlRegs.PIEACK.all = PIEACK_GROUP1;
}

__interrupt void cpu_timer0_isr(void){
	static float pastTheta = 0, pastThetaD = 0, aux5;
	long Counts;
	//////////////////////////////////////////////////						//////////////////////////////////////////////////
	//////////////////////////////////////////////////		Variables		//////////////////////////////////////////////////
	//////////////////////////////////////////////////						//////////////////////////////////////////////////
	time += Ts;
	//Position Calculation
	Counts =  -EQep1Regs.QPOSCNT; // Position in Counts 40000(counts)=2pi(rad)
	Theta = Counts*0.0001570796327L; // Position in Rad
	aux5 = time*time*time;
	ThetaD = aux5*((0.00037699L*time*time)-(0.0094248L*time)+0.062832L); //Desired Position (trajectory)
	//Speed Calculation
	DTheta = (Theta-pastTheta)*iTs; //Speed in Rad/s
	DThetaD = (ThetaD-pastThetaD)*iTs; //Speed in rad/s
	//////////////////////////////////////////////////						//////////////////////////////////////////////////
	//////////////////////////////////////////////////		Controller		//////////////////////////////////////////////////
	//////////////////////////////////////////////////						//////////////////////////////////////////////////
	Tau = -Kp*(Theta-ThetaD)-Kd*(DTheta-DThetaD);
	IaD = -Tau*sin(Nr*Theta)*kmI;
	IbD = Tau*cos(Nr*Theta)*kmI;
	Va = -AlphaA*(Ia-IaD)+R*IaD-km*DThetaD*sin(Nr*Theta);
	Vb = -AlphaB*(Ib-IbD)+R*IbD+km*DThetaD*cos(Nr*Theta);
	//Va = -sin(10*time)*Vmax; // 1500
	//Vb = cos(10*time)*Vmax;
	//////////////////////////////////////////////////						//////////////////////////////////////////////////
	//////////////////////////////////////////////////  Controller Output   //////////////////////////////////////////////////
	//////////////////////////////////////////////////						//////////////////////////////////////////////////
	if (time>=tf){
		SetPWMA(0);
		SetPWMB(0);
		//GpioDataRegs.GPATOGGLE.bit.GPIO13 = 1;
		StopCpuTimer0();
		asm(" ESTOP0");
	}
	else{
		SetPWMA(Va);
		SetPWMB(Vb);
	}
	pastTheta = Theta;
	pastThetaD= ThetaD;
	if (load == 0){
		ThetaArray[index] = Theta;
		DThetaArray[index] = DTheta;
		IaArray[index] = Ia;
		IbArray[index] = Ib;
		VaArray[index] = Va;
		VbArray[index++] = Vb;
		load++;
	}
	else {load = 0;}
	GpioDataRegs.GPATOGGLE.bit.GPIO13 = 1;
	PieCtrlRegs.PIEACK.all = PIEACK_GROUP1;
}

/* Calculate position and speed using Example_posspeed.c
POSSPEED qep_posspeed = POSSPEED_DEFAULTS;
qep_posspeed.init(&qep_posspeed);
qep_posspeed.mech_scaler = 524;
qep_posspeed.pole_pairs = 1;
qep_posspeed.calc(&qep_posspeed);// Position and Speed measurement
positionMechanical = qep_posspeed.theta_mech;
speedRPM = qep_posspeed.SpeedRpm_pr;
 */

/* Generate sine and cosine waves with PWM
static float index=0;
float sine, cosine, uT;
sine = sin(index)*EPwm7Regs.TBPRD;
cosine = cos(index)*EPwm9Regs.TBPRD;
EPwm7Regs.CMPA.bit.CMPA = EPwm7Regs.TBPRD - abs(sine);
EPwm9Regs.CMPA.bit.CMPA = EPwm9Regs.TBPRD - abs(cosine);
index += 0.01;
if (index > 6.2831) {index = 0;}
 */




/* Thesis Controller
static float gammakPD[N], gammakAD[N], ha, hb, Sigma2D, Sigma5D, Sigma2A, Sigma5A, Dg, g; // {0,0,0,0}
static float pastTheta, pastThetaD, pastDThetaD, pastDDThetaD;
static float pastGammakPD[N], pastGammakAD[N], pastSigma2D, pastSigma5D, pastG;
static float seno[N], cose[N];
float aux, sum, sum1, sum2, aux1, aux2, aux3, aux4, aux5;
int k;
long Counts;
//////////////////////////////////////////////////						//////////////////////////////////////////////////
//////////////////////////////////////////////////		Variables		//////////////////////////////////////////////////
//////////////////////////////////////////////////						//////////////////////////////////////////////////
time = time+Ts;
//Position Calculation
Counts = EQep1Regs.QPOSCNT;
Theta = Counts*0.0001570796327; //Position in Rad
aux5 = time*time*time;
ThetaD = aux5*(0.0000924*time*time-0.0023*time+0.0154); //Desired Position (trajectory)
//Speed Calculation
DTheta = (Theta-pastTheta)*iTs; //Speed in Rad/s
DThetaD = (ThetaD-pastThetaD)*iTs; //Speed in rad/s
//Acceleration calculation
DDThetaD = 0*(DThetaD-pastDThetaD)*iTs;
//Triple derivative calculation
DDDThetaD = 0*(DDThetaD-pastDDThetaD)*iTs;
//Load Torque and its derivative
g = 0;
Dg = (g-pastG)*iTs;
//////////////////////////////////////////////////						//////////////////////////////////////////////////
//////////////////////////////////////////////////		Controller		//////////////////////////////////////////////////
//////////////////////////////////////////////////						//////////////////////////////////////////////////
for (k=1;k<=N;k++){
	seno[k] = sin(k*Nr*Theta);
	cose[k] = cos(k*Nr*Theta);
}
for(k=1;k<N;k++){
	gammakP[k] = gammakP[k]+(gammakPD[k]+pastGammakPD[k])*Ts*0.5;
	gammakA[k] = gammakA[k]+(gammakAD[k]+pastGammakAD[k])*Ts*0.5;
}
sum = 0;
for(k=1;k<=N;k++){
	sum = sum+(gammakP[k]*cose[k]+gammakA[k]*seno[k]);
}
//Tau = -Kp*(Theta-ThetaD)-Kd*(DTheta-DThetaD)+sum+g+J*DDThetaD;
Tau = -Kp*(Theta-ThetaD)-Kd*(DTheta-DThetaD);
IaD = -Tau*seno[1]*kmC;
IbD = Tau*cose[1]*kmC;
aux = gamma*(Theta-ThetaD)-L*Kd*(Ia-IaD)*seno[1]*JkmC+L*Kd*(Ib-IbD)*cose[1]*JkmC+(DTheta-DThetaD);
for(k=1;k<=N;k++){
	gammakPD[k] = -GammakP*aux*cose[k];
	gammakAD[k] = -GammakA*aux*seno[k];
}
Sigma2D = -Gamma2*(Ia-IaD)*Tau*DTheta*cose[1];
Sigma5D = -Gamma5*(Ib-IbD)*Tau*DTheta*seno[1];
Sigma2A = Sigma2+(Sigma2D+pastSigma2D)*Ts*0.5;
Sigma5A = Sigma5+(Sigma5D+pastSigma5D)*Ts*0.5;
Sigma2 = Sigma2A*Tau*DTheta;
Sigma5 = Sigma5A*Tau*DTheta;
sum1 = 0;
sum2 = 0;
for (k=1;k<=N;k++){
	aux1 = gammakPD[k]*cose[k]+gammakAD[k]*seno[k]+gammakA[k]*k*DTheta*cose[k];
	aux2 = gammakP[k]*k*DTheta*seno[k];
	aux3 = aux1+aux2;
	aux4 = aux1-aux2;
	sum1 = sum1+aux3;
	sum2 = sum2+aux4;
}
ha = -(L/km)*(sum1+Dg+J*DDThetaD)*seno[1];
hb = (L/km)*(sum2+Dg+J*DDThetaD)*cose[1];
//Va = -AlphaA*(Ia-IaD)+Sigma2*cose[1]+R*IaD-km*DThetaD*seno[1]+ha;
//Vb = -AlphaB*(Ib-IbD)+Sigma5*seno[1]+R*IbD+km*DThetaD*cose[1]+hb;
Va = -AlphaA*(Ia-IaD);
Vb = -AlphaB*(Ib-IbD);
//////////////////////////////////////////////////						//////////////////////////////////////////////////
//////////////////////////////////////////////////  Controller Output   //////////////////////////////////////////////////
//////////////////////////////////////////////////						//////////////////////////////////////////////////
SetPWMA(Va);
SetPWMB(Vb);
if (time>=tf){
	SetPWMA(0);
	SetPWMB(0);
	GpioDataRegs.GPATOGGLE.bit.GPIO13 = 1;
	asm(" ESTOP0");
}
pastTheta = Theta;
pastThetaD= ThetaD;
pastDThetaD = DThetaD;
pastDDThetaD = DDThetaD;
for(k=1;k<N;k++){
	pastGammakPD[k] = gammakPD[k];
	pastGammakAD[k] = gammakAD[k];
}
pastSigma2D = Sigma2D;
pastSigma5D = Sigma5D;
pastG = g;
PieCtrlRegs.PIEACK.all = PIEACK_GROUP1;
 */
