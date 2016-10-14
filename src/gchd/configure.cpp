/**
 * Copyright (c) 2014 - 2015 Tolga Cakir <tolga@cevel.net>
 *
 * This source file is part of Game Capture HD Linux driver and is distributed
 * under the MIT License. For more information, see LICENSE file.
 */

#include <vector>
#include "../gchd.hpp"


//This runs the commands for all configurations up until a point they diverge
//badly in a way we haven't been unable to untangle yet.
void GCHD::configureDevice()
{
	std::vector<unsigned char> version;
	readVersion( version );
	printf("Firmware Version is %s\n", version.data());

	//register is named BANKSEL from script files.
	write_config<uint16_t>(BANKSEL, 0x0000);

	//Get what is enabled in case we aren't in boot state, we may need to disable
	savedEnableStateRegister_ = read_config<uint16_t>(MAIL_SEND_ENABLE_REGISTER_STATE);

	//Okay, when we first read SCMD_STATE_READBACK_REGISTER
	//To get our current state, it actually TRIGGERS a state change.
	//but only in the case that the current state
	//(as read from SCMD_STATE_READBACK_REGISTER)
	//is already 0.
	uint16_t state;
	if( deviceType_ == DeviceType::GameCaptureHD )
	{
		//Since currentState and nextState are set to 0,
		//This will error out without waiting for the state
		//change to complete if we are in another state
		//which means the flash is loaded.
		//The error will be signified by state != 0
		//Otherwise it will do the transition.
		state=completeStateChange(0x0000, 0x0000);
	}
	else if( deviceType_ == DeviceType::GameCaptureHDNew )
	{
		//We can't use completeStateChange for HDNew,
		//as the read triggers an interrupt, which
		//the normal completeStateChange code doesn't handle.
		bool changed;
		state=read_config<uint16_t>(SCMD_STATE_READBACK_REGISTER);
		state &= 0x1f; //Ignore any other bits other than the ones we care about.

		if( state == 0x0000 )
		{
			interruptPend();
			do
			{
				uint16_t completion=read_config<uint16_t>(SCMD_STATE_CHANGE_COMPLETE);
				changed = (completion & 0x4)>0; //Check appropriate bit
			} while (!changed);

			//Reset sticky bit/acknowledge.
			write_config<uint16_t>(SCMD_STATE_CHANGE_COMPLETE, 0x0004);
		}
	}
	if( state == 0x0000) //We have to load the flash
	{
		// load "idle" firmware
		dlfirm(firmwareIdle_.c_str());

		//no idea what this does.
		write_config<uint16_t>(0xbc, 0x0900, 0x0070, 0x0004);

		savedEnableStateRegister_= read_config<uint16_t>(MAIL_SEND_ENABLE_REGISTER_STATE);
		savedEnableRegister_= read_config<uint16_t>(ENABLE_REGISTER);

		//Not sure what any of this is done for, but it appears we read 2 banks
		// of things
		//that are identical.
		read_config(0xbc, 0x0000, 0x0010, 2); //EXPECTED=0x20, 0x13
		read_config(0xbc, 0x0000, 0x0012, 2); //EXPECTED=0x12, 0x10
		read_config(0xbc, 0x0000, 0x0014, 2); //EXPECTED=0x18, 0x80
		read_config(0xbc, 0x0000, 0x0016, 2); //EXPECTED=0x20, 0x30
		read_config(0xbc, 0x0000, 0x0018, 2); //EXPECTED=0x20, 0x13
		read_config(0xbc, 0x0000, 0x001a, 2); //EXPECTED=0x12, 0x10
		read_config(0xbc, 0x0000, 0x001c, 2); //EXPECTED=0x18, 0x80
		read_config(0xbc, 0x0000, 0x001e, 2); //EXPECTED=0x20, 0x30
	}
	else
	{
		/* Oh, we were already up. Reset to known state, and flash
	 * doesn't need to be loaded
	 */
		stateConfirmedScmd( SCMD_RESET, 0x00, 0x0000 );
	}
	stateConfirmedScmd( SCMD_IDLE, 0x00, 0x0000 );

	specialDetectMask_ = 0xffff; //We are going to use this to auto-detect what
	//type of signal is incoming.

	//Confirm that we are up
	mailWrite( 0x33, VC{0xab, 0xa9, 0x0f, 0xa4, 0x55} );
	mailRead( 0x33, 3 ); //EXPECTED {0x33, 0x44, 0x55}

	std::vector<unsigned char> input=std::vector<unsigned char>(0);

	bool loopDone=false;
	uint32_t deviceModeMagic;
	bool firstTime=true;

	while(!loopDone)
	{
		mailWrite( 0x33, {0xab, 0xa9, 0x0f, 0xa4, 0x55} );
		input=mailRead( 0x33, 3 ); /* read 3 bytes from 0x33 */

		//Get magic number requested from port 0x33 last state.
		//Convert 3 bytes into one 24 bit number.
		deviceModeMagic=
				Utility::debyteify<uint32_t>(input.data(), 3);


		switch( deviceModeMagic ) {
			case 0x334455: //First device magic number we read back,
				//We loop on this till it transitions to 0x27f97b
			{
				//TURN ON PROCESSOR
				sendEnableState();
				enableAnalogInput();
				doEnable( EB_FIRMWARE_PROCESSOR, EB_FIRMWARE_PROCESSOR );
			}
				break;

			case 0x27f97b:
			{
				if ((settings_->getInputSource() == InputSource::Unknown) && firstTime ) {
					bool analogSignalFound = ((specialDetectMask_ >> 5) & 1);
					bool hdmiSignalFound = (( specialDetectMask_ >> 3) & 1) != 0;
					unsigned cableType = specialDetectMask_  & 3;
					bool signalFound;

					if( cableType == 0 ) {
						signalFound=hdmiSignalFound;
					} else {
						signalFound=analogSignalFound;
					}

					if( !signalFound ) {
						printf("No signal found. Defaulting to HDMI\n");
						settings_->setInputSource(InputSource::HDMI);
					} else {
						switch(cableType)
						{
							case 3:
								printf("Composite signal found.\n");
								settings_->setInputSource(InputSource::Composite);
								break;
							case 2:
								printf("Component signal found.\n");
								settings_->setInputSource(InputSource::Component);
								break;
							case 0:
								printf("HDMI input signal found.\n");
								settings_->setInputSource(InputSource::HDMI);
								break;

								break;
							default:
								throw runtime_error("Bad cable detection code.");
						}
					}
				}

				//THis number is the 2nd number we read
				//back, and we end up repeating this after
				//main_initialize until the initialization is done.
				//Then we transition to 0x78e045, and we then come back here.
				enableAnalogInput();

				if( firstTime )  {

					transcoderDefaultsInitialize();         //just came back from here.
					scmd(SCMD_INIT, 0x00, 0x0000);

					dlfirm(firmwareEnc_.c_str());

					firstTime=false;
					read_config<uint16_t>(0xbc, 0x0000, 0x0010); //EXPECTED=0x2013
					read_config<uint16_t>(0xbc, 0x0000, 0x0012); //EXPECTED=0x1210
					read_config<uint16_t>(0xbc, 0x0000, 0x0014); //EXPECTED=0x1880
					read_config<uint16_t>(0xbc, 0x0000, 0x0016); //EXPECTED=0x2030
					//Note that it is possible after firmware load for us to fallback
					//temporarily to 0x334455 on some devices.

				} else {
					loopDone=true;
				}

			}
				break;
		}
	}
	do {
		mailWrite( 0x33, {0xab, 0xa9, 0x0f, 0xa4, 0x5b} );
		input=mailRead( 0x33, 3 ); /* read 3 bytes from 0x33 */
		enableAnalogInput();
		deviceModeMagic=
				Utility::debyteify<uint32_t>(input.data(), 3);
	} while(deviceModeMagic != 0x78e045);
	doEnable( EB_ENCODER_ENABLE, EB_ENCODER_ENABLE );

	//We can go back to 0x334455 after we turn on EB_ENCODER_ENABLE in some cases,
	//Wait till we are 0x27f97b
	do
	{
		mailWrite( 0x33, {0xab, 0xa9, 0x0f, 0xa4, 0x55} );
		input=mailRead( 0x33, 3 ); /* read 3 bytes from 0x33 */
		deviceModeMagic=
				Utility::debyteify<uint32_t>(input.data(), 3);
	} while( deviceModeMagic != 0x27f97b );

	mailWrite( 0x33, VC{0x28, 0x28} );
	mailWrite( 0x33, VC{0x29, 0x89, 0x5b} );
	mailRead( 0x33, 1 ); //EXPECTED {0x91}
	mailWrite( 0x33, VC{0xdd, 0xce, 0x3f, 0xb2} );
	mailRead( 0x33, 2 ); //EXPECTED {0xda, 0x67}

	doEnable( EB_ENCODER_TRIGGER, EB_ENCODER_TRIGGER ); //Turn on trigger bit
	do
	{
		mailWrite( 0x33, VC{0x43, 0x23, 0x84} );
		input=mailRead( 0x33, 1 ); //EXPECTED {0xf7}
	} while(input[0] != 0xf7);
	doEnable( EB_ENCODER_TRIGGER, 0 ); //Turn off trigger bit

	mailWrite( 0x33, VC{0x89, 0x89, 0xfb} );
	mailRead( 0x33, 1 ); //EXPECTED {0x6e}

	//Potential subroutine
	{
		mailWrite( 0x44, VC{0x02, 0xc9} );
		mailWrite( 0x44, VC{0x14, 0xd2} );
		mailWrite( 0x44, VC{0x3c, 0x6b} );
		mailWrite( 0x33, VC{0x89, 0x89, 0xfa} );
		mailRead( 0x33, 1 ); //EXPECTED {0xed}
	}
	mailWrite( 0x33, VC{0x89, 0x89, 0xca} );
	mailRead( 0x33, 1 ); //EXPECTED {0xee}
	mailWrite( 0x33, VC{0x89, 0x89, 0xe7} );
	mailRead( 0x33, 1 ); //EXPECTED {0x49}
	mailWrite( 0x44, VC{0x03, 0x2a} );
	mailWrite( 0x44, VC{0x05, 0x89} );

	//This is an educated guess right now as to why it
	//set one way or the other. Currently it presumed
	//that it doesn't really matter much because
	//the old configure scripts didn't seem to match
	//captures that I've seen.
	bool analog = settings_->getInputSource() != InputSource::HDMI;
	if( analog ) {
		mailWrite( 0x44, VC{0x08, 0x91} );
		mailWrite( 0x44, VC{0x09, 0xa8} );
	} else {
		mailWrite( 0x44, VC{0x08, 0x9b} );
		mailWrite( 0x44, VC{0x09, 0x7a} );
	}
	mailWrite( 0x44, VC{0x19, 0xde} );
	mailWrite( 0x44, VC{0x1a, 0x87} );
	mailWrite( 0x44, VC{0x1b, 0x88} );
	mailWrite( 0x44, VC{0x29, 0x8b} );
	mailWrite( 0x44, VC{0x2d, 0x8f} );
	mailWrite( 0x44, VC{0x4c, 0x89} );
	mailWrite( 0x44, VC{0x55, 0x88} );
	mailWrite( 0x44, VC{0x6b, 0xae} );
	mailWrite( 0x44, VC{0x6c, 0xbe} );
	mailWrite( 0x44, VC{0x6d, 0x78} );
	mailWrite( 0x44, VC{0x6e, 0xa0} );
	mailWrite( 0x44, VC{0x06, 0x08} );

	//Potential subroutine
	{
		mailWrite( 0x44, VC{0x02, 0xc9} );
		mailWrite( 0x44, VC{0x14, 0xd2} );
		mailWrite( 0x44, VC{0x3c, 0x6b} );
		mailWrite( 0x33, VC{0x89, 0x89, 0xfa} );
		mailRead( 0x33, 1 ); //EXPECTED {0xfd}
	}
	mailWrite( 0x44, VC{0x28, 0x88} );
	mailWrite( 0x44, VC{0x10, 0x88} );
	mailWrite( 0x44, VC{0x11, 0xd4} );
	mailWrite( 0x44, VC{0x12, 0xd0} );
	mailWrite( 0x44, VC{0x13, 0x08} );
	mailWrite( 0x44, VC{0x14, 0x08} );
	mailWrite( 0x44, VC{0x15, 0x88} );
	mailWrite( 0x33, VC{0x94, 0x47, 0xf9} );
	mailWrite( 0x33, VC{0x94, 0x40, 0xf3} );
	mailWrite( 0x33, VC{0x94, 0x43, 0xb7} );
	mailWrite( 0x33, VC{0x94, 0x4e, 0xb7} );
	mailWrite( 0x33, VC{0x94, 0x4f, 0xb7} );
	mailWrite( 0x33, VC{0x94, 0x48, 0xb7} );
	mailWrite( 0x33, VC{0x94, 0x49, 0xb7} );
	mailWrite( 0x33, VC{0x94, 0x58, 0x77} );
	mailWrite( 0x33, VC{0x94, 0x40, 0xf1} );
	mailWrite( 0x33, VC{0x94, 0x4d, 0xf5} );
	mailWrite( 0x33, VC{0x94, 0x4a, 0xaf} );
	mailWrite( 0x33, VC{0x94, 0x4b, 0xaf} );
	mailWrite( 0x33, VC{0x94, 0x5c, 0xb7} );
	mailWrite( 0x33, VC{0x94, 0x46, 0xd7} );

	readDevice0x9DCD(0x88); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0xb7, 0xce} );
	mailWrite( 0x4e, VC{0x41, 0xa3} );
	mailWrite( 0x4e, VC{0xb8, 0xcc} );
	readDevice0x9DCD(0x3f); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0x00, 0xcd} );
	mailWrite( 0x4e, VC{0x0f, 0xce} );
	mailWrite( 0x4e, VC{0x16, 0xfc} );
	mailWrite( 0x4e, VC{0x17, 0xcc} );
	mailWrite( 0x4e, VC{0x18, 0xcc} );
	mailWrite( 0x4e, VC{0x19, 0xcc} );
	mailWrite( 0x4e, VC{0x1a, 0x9c} );
	readDevice0x9DCD(0x15); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0x2a, 0xcb} );
	readDevice0x9DCD(0x3f); //EXPECTED 0xb3
	mailWrite( 0x4e, VC{0x00, 0xce} );
	mailWrite( 0x4e, VC{0x08, 0xcf} );
	readDevice0x9DCD(0x3f); //EXPECTED 0xb0
	mailWrite( 0x4e, VC{0x00, 0xcd} );
	if ( deviceType_ == DeviceType::GameCaptureHDNew )
	{
		mailWrite( 0x4e, VC{0x24, 0x8c} );
	} else {
		mailWrite( 0x4e, VC{0x24, 0x8d} );
	}
	mailWrite( 0x4e, VC{0x25, 0xcc} );
	mailWrite( 0x4e, VC{0x30, 0x4c} );
	mailWrite( 0x4e, VC{0x31, 0xcc} );
	mailWrite( 0x4e, VC{0x32, 0xcc} );
	mailWrite( 0x4e, VC{0x25, 0xcc} );
	mailWrite( 0x4e, VC{0x26, 0xcc} );
	mailWrite( 0x4e, VC{0x27, 0xcc} );
	mailWrite( 0x4e, VC{0x27, 0xcc} );
	mailWrite( 0x4e, VC{0x27, 0xcc} );
	mailWrite( 0x4e, VC{0x27, 0xcc} );
	mailWrite( 0x4e, VC{0x27, 0xcc} );
	readDevice0x9DCD(0x3f); //EXPECTED 0xb3
	mailWrite( 0x4e, VC{0x00, 0xcc} );
	mailWrite( 0x4e, VC{0xb0, 0xe8} );
	readDevice0x9DCD(0x91); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0xae, 0xc8} );
	mailWrite( 0x4e, VC{0xb1, 0x0c} );
	mailWrite( 0x4e, VC{0xb2, 0xcc} );
	mailWrite( 0x4e, VC{0xb3, 0xcc} );
	mailWrite( 0x4e, VC{0xb4, 0x99} );
	readDevice0x9DCD(0x8b); //EXPECTED 0xe7
	mailWrite( 0x4e, VC{0xb4, 0x98} );
	readDevice0x9DCD(0x3f); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0x00, 0xce} );
	mailWrite( 0x4e, VC{0x01, 0xad} );
	mailWrite( 0x4e, VC{0x02, 0x39} );
	readDevice0x9DCD(0x3c); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0x03, 0xce} );
	mailWrite( 0x4e, VC{0x04, 0xcd} );
	mailWrite( 0x4e, VC{0x05, 0xcc} );
	mailWrite( 0x4e, VC{0x06, 0xc4} );
	mailWrite( 0x4e, VC{0x1c, 0xd6} );
	mailWrite( 0x4e, VC{0x1d, 0xcc} );
	mailWrite( 0x4e, VC{0x1e, 0xcc} );
	mailWrite( 0x4e, VC{0x1f, 0xcc} );
	readDevice0x9DCD(0x1a); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0x25, 0x6e} );
	readDevice0x9DCD(0x3d); //EXPECTED 0x47
	mailWrite( 0x4e, VC{0x02, 0x39} );
	readDevice0x9DCD(0x38); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0x07, 0xc8} );
	mailWrite( 0x4e, VC{0x17, 0x0c} );
	mailWrite( 0x4e, VC{0x19, 0x33} );
	mailWrite( 0x4e, VC{0x1a, 0x33} );
	mailWrite( 0x4e, VC{0x1b, 0x30} );
	mailWrite( 0x4e, VC{0x20, 0xcc} );
	readDevice0x9DCD(0x1e); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0x21, 0xcc} );
	mailWrite( 0x4e, VC{0x22, 0xea} );
	mailWrite( 0x4e, VC{0x27, 0xcc} );
	readDevice0x9DCD(0x11); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0x2e, 0x6d} );
	mailWrite( 0x33, VC{0x99, 0x89, 0xfa} );
	mailRead( 0x33, 1 ); //EXPECTED {0xa4}
	mailWrite( 0x33, VC{0x99, 0x89, 0xf9} );
	mailRead( 0x33, 1 ); //EXPECTED {0x7f}
	mailWrite( 0x33, VC{0x99, 0x89, 0xf8} );
	mailRead( 0x33, 1 ); //EXPECTED {0x78}
	mailWrite( 0x33, VC{0x99, 0x89, 0xfe} );
	mailRead( 0x33, 1 ); //EXPECTED {0x0e}
	mailWrite( 0x4c, VC{0x05, 0x88} );
	mailWrite( 0x4c, VC{0x04, 0xb5} );
	mailWrite( 0x4c, VC{0x04, 0x95} );
	mailWrite( 0x4c, VC{0x61, 0xb8} );
	mailWrite( 0x4c, VC{0x09, 0x3a} );
	mailWrite( 0x4c, VC{0x0a, 0x70} );
	mailWrite( 0x4c, VC{0x0b, 0xbf} );
	mailWrite( 0x4c, VC{0xc9, 0x88} );
	mailWrite( 0x4c, VC{0xca, 0x88} );
	mailWrite( 0x4c, VC{0xcb, 0x88} );
	mailWrite( 0x4c, VC{0xcc, 0x88} );
	mailWrite( 0x4c, VC{0xcd, 0x88} );
	mailWrite( 0x4c, VC{0xce, 0x88} );
	mailWrite( 0x4c, VC{0xcf, 0x88} );
	mailWrite( 0x4c, VC{0xd0, 0x88} );

	if ( deviceType_ == DeviceType::GameCaptureHDNew )
	{
		mailWrite( 0x33, VC{0x21, 0x01, 0x72} );
		mailRead( 0x33, 1 ); //EXPECTED {0xf4}
		mailWrite( 0x33, VC{0x20, 0x02, 0x63} );
		mailWrite( 0x33, VC{0x20, 0x03, 0x63} );
		mailWrite( 0x33, VC{0x20, 0x04, 0x77} );
		mailWrite( 0x33, VC{0x20, 0x05, 0x73} );
		mailWrite( 0x33, VC{0x20, 0x06, 0x73} );
		mailWrite( 0x33, VC{0x20, 0x07, 0x33} );
		mailWrite( 0x33, VC{0x20, 0x08, 0x31} );
		mailWrite( 0x33, VC{0x20, 0x09, 0x33} );
		mailWrite( 0x33, VC{0x20, 0x0a, 0x57} );
		mailWrite( 0x33, VC{0x20, 0x0b, 0x7b} );
		mailWrite( 0x33, VC{0x20, 0x0c, 0xf7} );
		mailWrite( 0x33, VC{0x20, 0x0d, 0xf7} );
		mailWrite( 0x33, VC{0x20, 0x0e, 0x73} );
		mailWrite( 0x33, VC{0x20, 0x0f, 0x73} );
	}
	mailWrite( 0x33, VC{0xaa, 0x8f, 0x3b} );


	//----------------------------------------------------------

	//The next set of writes and then 56 byte reads are not understood at
	//all, but it is hypothesized that the values read back tell us
	//information about the video signal coming in, and are used to
	//autodetect incoming signal type.
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x03, 0x76} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x3b, 0x76} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x73, 0x76} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xab, 0x76} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xe3, 0x76} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x1b, 0x77} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x53, 0x77} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x8b, 0x77} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xc3, 0x77} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xfb, 0x77} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x33, 0x74} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x6b, 0x74} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xa3, 0x74} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xdb, 0x74} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x13, 0x75} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x4b, 0x75} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x83, 0x75} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xbb, 0x75} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xf3, 0x75} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x2b, 0x72} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x63, 0x72} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x9b, 0x72} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xd3, 0x72} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x0b, 0x73} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x43, 0x73} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x7b, 0x73} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xb3, 0x73} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xeb, 0x73} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x23, 0x70} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x5b, 0x70} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x93, 0x70} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xcb, 0x70} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x03, 0x71} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x3b, 0x71} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x73, 0x71} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xab, 0x71} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xe3, 0x71} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x1b, 0x7e} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x53, 0x7e} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x8b, 0x7e} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xc3, 0x7e} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xfb, 0x7e} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x33, 0x7f} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x6b, 0x7f} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xa3, 0x7f} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xdb, 0x7f} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x13, 0x7c} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x4b, 0x7c} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x83, 0x7c} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xbb, 0x7c} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xf3, 0x7c} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x2b, 0x7d} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x63, 0x7d} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x9b, 0x7d} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xd3, 0x7d} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x0b, 0x7a} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x43, 0x7a} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x7b, 0x7a} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xb3, 0x7a} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xeb, 0x7a} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x23, 0x7b} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x5b, 0x7b} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x93, 0x7b} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xcb, 0x7b} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x03, 0x78} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x3b, 0x78} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x73, 0x78} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xab, 0x78} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xe3, 0x78} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x1b, 0x79} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x53, 0x79} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0x8b, 0x79} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0x92, 0x3e, 0xb4, 0xc3, 0x79} );
	mailRead( 0x33, 56 );
	mailWrite( 0x33, VC{0xab, 0xa2, 0x3e, 0xb4, 0xfb, 0x79} );


	mailRead( 0x33, 8 ); //EXPECTED {0xe9, 0x5c, 0xcf, 0x42, 0xb5, 0x28, 0x9b, 0x0e}
	mailWrite( 0x33, VC{0xaa, 0x8d, 0x35} );
	do {
		mailWrite( 0x33, {0xab, 0xa9, 0x0f, 0xa4, 0x5b} );
		input=mailRead( 0x33, 3 ); /* read 3 bytes from 0x33 */
		enableAnalogInput();
		deviceModeMagic=
				Utility::debyteify<uint32_t>(input.data(), 3);
	} while(deviceModeMagic != 0x78e045);

	transcoderSetup();
	transcoderOutputEnable(true);

	analog = settings_->getInputSource() != InputSource::HDMI;
	bool composite = settings_->getInputSource() == InputSource::Composite;

	scmd(SCMD_INIT, 0xa0, 0x0000);
	if( !composite )  {
		mailWrite( 0x44, VC{0x06, 0x86} );
	} else {
		mailWrite( 0x33, VC{0x89, 0x89, 0xfd} );
		mailRead( 0x33, 1 ); //EXPECTED {0x6e}
	}
	mailWrite( 0x33, VC{0x89, 0x89, 0xf8} );
	mailRead( 0x33, 1 ); //EXPECTED {0xcc}

	if( !composite ) {
		mailWrite( 0x44, VC{0x03, 0x2f} );
	} else {
		mailWrite( 0x44, VC{0x03, 0x28} );
	}
	readDevice0x9DCD(0x3f); //EXPECTED 0xb0
	mailWrite( 0x4e, VC{0x00, 0xcc} );
	if( !composite ) {
		mailWrite( 0x4e, VC{0xb3, 0xcc} );
	} else {
		mailWrite( 0x4e, VC{0xb3, 0x33} );
	}
	readDevice0x9DCD(0x3f); //EXPECTED 0xb2
	mailWrite( 0x4e, VC{0x00, 0xce} );
	if( !composite ) {
		mailWrite( 0x4e, VC{0x27, 0xcc} );
	} else {
		mailWrite( 0x4e, VC{0x27, 0x33} );
		readDevice0x9DCD(0x3f);
		mailWrite( 0x4e, VC{0x00, 0xcc} );
		readDevice0x9DCD(0x6e);
		mailWrite( 0x4e, VC{0x51, 0xcc} );
	}
	doEnable( EB_COMPOSITE_MUX, composite ? EB_COMPOSITE_MUX: 0);
	doEnable( EB_ANALOG_INPUT, analog ? EB_ANALOG_INPUT: 0);
	doEnable( EB_ANALOG_MUX, analog ? EB_ANALOG_MUX:0);


	switch (settings_->getInputSource())
	{
		case InputSource::HDMI:
			configureHDMI();
			break;
#if 0
		case InputSource::Composite:
			setupComposite();
			break;
		case InputSource::Component:
			setupComponent():
				break;
#endif
		case InputSource::Unknown:
		default:
			throw runtime_error("Unknown input source not currently allowed.");
			break;
	}
}


void GCHD::uninitDevice()
{
	uint16_t state=read_config<uint16_t>(SCMD_STATE_READBACK_REGISTER) & 0x1f;
	if(( state == SCMD_STATE_START ) || ( state==SCMD_STATE_NULL )) {
		stopStream( true );
	}

	//0x12 means already unininitialized (SCMD_RESET with mode=0x1),
	//0x10 means already unininitialized (SCMD_RESET with mode=0x0).
	//ox00 means never initialized.
	if(( state != 0x12 ) && (state != 0x00) && (state != 0x10)) {
		//Mystery subroutine, done after an SCMD_INIT too.
		{
			mailWrite( 0x44, VC{0x06, 0x86} );
			mailWrite( 0x33, VC{0x89, 0x89, 0xf8} );
			mailRead( 0x33, 1 ); //EXPECTED {0xc9}
			mailWrite( 0x44, VC{0x03, 0x2f} );
		}
		//No idea what this is, but presumably selects proper bank.
		//Seems to be always done before configuring the transcoder with
		//sparam commands.
		write_config<uint16_t>(BANKSEL, 0x0000);
		readEnableState(); //EXPECTED 0xd39e HD NEW. 0x31e on HD

		read_config<uint16_t>(SCMD_STATE_READBACK_REGISTER); //seems no reason for this read..
		transcoderOutputEnable( false );
		scmd(SCMD_INIT, 0xa0, 0x0000);

		clearEnableState();

		//This command appears to do nothing, it appears to be a doEnable
		//for a bit that we can't identify, that isn't configured
		//in any of our test cases.
		doEnable(EB_FIRMWARE_PROCESSOR, 0x0);

		stateConfirmedScmd( SCMD_IDLE, 0x00, 0x0000 );
		stateConfirmedScmd( SCMD_RESET, 0x01, 0x0000 );
	}
}
