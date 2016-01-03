// There are 2 ways sound can be handled here.
//
// If "hyperSound" (aka "Sound Fix") is enabled, each piece of sound is 
// synchronized with ds hardware. This allows for very accurate emulation of 
// sound effects like Pikachu.
//
// If "hyperSound" is not enabled, sound is not synchronized to the cycle, it is 
// simply played as soon as it is computed.

#include <nds.h>
#include <nds/fifomessages.h>
#include <time.h>
#include "mmu.h"
#include "console.h"
#include "gameboy.h"
#include "soundengine.h"
#include "common.h"
#include "menu.h"

const DutyCycle dutyIndex[4] = {DutyCycle_12, DutyCycle_25, DutyCycle_50, DutyCycle_75};

// If this many fifo commands have been sent but not received, skip further 
// commands. This helps prevent crashing.
#define MAX_FIFOS_WAITING 60

inline void FIFO_SEND(u32 message) {
    if (sharedData->fifosSent-sharedData->fifosReceived < MAX_FIFOS_WAITING) {
        sharedData->fifosSent++;
        fifoSendValue32(FIFO_USER_01, message);
    }
    else {
        printLog("Sound error\n");
    }
}

// SoundEngine functions

SoundEngine::SoundEngine(Gameboy* g) {
    setGameboy(g);
    sampleData = (u8*)memUncached(malloc(0x20));

    unmute();
}

SoundEngine::~SoundEngine() {
    free(sampleData);
}

void SoundEngine::setGameboy(Gameboy* g) {
    gameboy = g;
}

void SoundEngine::init()
{
    static double analog[] = { -1, -0.8667, -0.7334, -0.6, -0.4668, -0.3335, -0.2, -0.067, 0.0664, 0.2, 0.333, 0.4668, 0.6, 0.7334, 0.8667, 1  } ;
    int i;
    for (i=0; i<16; i++)
    {
        pcmVals[i] = analog[i]*0x70;
    }
    chan1SweepTime = 0;
    chan1SweepCounter = 0;
    chan1SweepDir = 0;
    chan1SweepAmount = 0;
    memset(chanLen, 0, sizeof(chanLen));
    memset(chanLenCounter, 0, sizeof(chanLenCounter));
    memset(chanUseLen, 0, sizeof(chanUseLen));
    memset(chanFreq, 0, sizeof(chanFreq));
    memset(chanVol, 0, sizeof(chanVol));
    memset(chanEnvDir, 0, sizeof(chanEnvDir));
    memset(chanEnvCounter, 0, sizeof(chanEnvCounter));
    memset(chanEnvSweep, 0, sizeof(chanEnvSweep));
    memset(chanVol, 0, sizeof(chanVol));

    refresh();
}

void SoundEngine::refresh() {

    soundEnable();
    unmute();
    if (soundDisabled)
        return;

    // Send the cached mirror to preserve dsi compatibility.
    // Because of this, on arm9, always use the local sampleData which is 
    // uncached.
    sharedPtr->sampleData = (u8*)memCached(sampleData);
    sharedPtr->chanOn = 0;

    // Ordering note: Writing a byte to FF26 with bit 7 set enables writes to
    // the other registers. With bit 7 unset, writes are ignored.
    handleSoundRegister(0x26, gameboy->readIO(0x26));

    for (int i=0x10; i<=0x3F; i++) {
        if (i == 0x14 || i == 0x19 || i == 0x1e || i == 0x23)
            // Don't restart the sound channels.
            handleSoundRegister(i, gameboy->readIO(i)&~0x80);
        else
            handleSoundRegister(i, gameboy->readIO(i));
    }

    if (gameboy->readIO(0x26) & 1)
        handleSoundRegister(0x14, gameboy->readIO(0x14)|0x80);
    if (gameboy->readIO(0x26) & 2)
        handleSoundRegister(0x19, gameboy->readIO(0x19)|0x80);
    if (gameboy->readIO(0x26) & 4)
        handleSoundRegister(0x1e, gameboy->readIO(0x1e)|0x80);
    if (gameboy->readIO(0x26) & 8)
        handleSoundRegister(0x23, gameboy->readIO(0x23)|0x80);
}

void SoundEngine::mute() {
    // sharedPtr accesses won't affect the main soundEngine
    muted = true;
    sharedPtr = dummySharedData;
}
void SoundEngine::unmute() {
    muted = false;
    sharedPtr = sharedData;
}

void SoundEngine::setSoundEventCycles(int cycles) {
    if (cyclesToSoundEvent > cycles) {
        cyclesToSoundEvent = cycles;
    }
}

void SoundEngine::updateSound(int cycles)
{
    if (soundDisabled)
        return;
    bool changed=false;
    if ((sharedPtr->chanOn & CHAN_1) && chan1SweepTime != 0)
    {
        chan1SweepCounter -= cycles;
        while (chan1SweepCounter <= 0)
        {
            chan1SweepCounter += (clockSpeed/(128/chan1SweepTime))+chan1SweepCounter;
            chanFreq[0] += (chanFreq[0]>>chan1SweepAmount)*chan1SweepDir;
            if (chanFreq[0] > 0x7FF)
            {
                sharedPtr->chanOn &= ~CHAN_1;
                gameboy->clearSoundChannel(CHAN_1);
            }
            else {
                refreshSoundFreq(0);
            }
            changed = true;
        }

        if (sharedPtr->chanOn & CHAN_1)
            setSoundEventCycles(chan1SweepCounter);
    }
    for (int i=0; i<4; i++)
    {
        if (i != 2 && sharedPtr->chanOn & (1<<i))
        {
            if (chanEnvSweep[i] != 0)
            {
                chanEnvCounter[i] -= cycles;
                while (chanEnvCounter[i] <= 0)
                {
                    chanEnvCounter[i] += chanEnvSweep[i]*clockSpeed/64;
                    chanVol[i] += chanEnvDir[i];
                    if (chanVol[i] < 0)
                        chanVol[i] = 0;
                    if (chanVol[i] > 0xF)
                        chanVol[i] = 0xF;
                    changed = true;
                    refreshSoundVolume(i);
                }
                if (chanVol[i] != 0 && chanVol[i] != 0xF)
                    setSoundEventCycles(chanEnvCounter[i]);
            }
        }
    }
    for (int i=0; i<4; i++)
    {
        if ((sharedPtr->chanOn & (1<<i)) && chanUseLen[i])
        {
            chanLenCounter[i] -= cycles;
            if (chanLenCounter[i] <= 0)
            {
                sharedPtr->chanOn &= ~(1<<i);
                changed = true;
                if (i==0)
                    gameboy->clearSoundChannel(CHAN_1);
                else if (i == 1)
                    gameboy->clearSoundChannel(CHAN_2);
                else if (i == 2)
                    gameboy->clearSoundChannel(CHAN_3);
                else
                    gameboy->clearSoundChannel(CHAN_4);
            }
            else
                setSoundEventCycles(chanLenCounter[i]);
        }
    }
    if (changed) {
        if (hyperSound)
            sendUpdateMessage(-1);
        else {
            if (muted)
                return;
            // Force immediate update, even though hyperSound isn't on.
            FIFO_SEND(GBSND_UPDATE_COMMAND<<28 | 4);
        }
    }
}

void SoundEngine::soundUpdateVBlank() {
    // This debug stuff helps when debugging Pokemon Diamond
    //printLog("%d\n", sharedPtr->fifosSent-sharedPtr->fifosReceived);
}

void SoundEngine::handleSoundRegister(u8 ioReg, u8 val)
{
    switch (ioReg)
    {
        // CHANNEL 1
        // Sweep
        case 0x10:
            chan1SweepTime = (val>>4)&0x7;
            if (chan1SweepTime != 0) {
                chan1SweepCounter = clockSpeed/(128/chan1SweepTime);
                setSoundEventCycles(chan1SweepCounter);
            }
            chan1SweepDir = (val&0x8) ? -1 : 1;
            chan1SweepAmount = (val&0x7);
            break;
            // Length / Duty
        case 0x11:
            {
                chanLen[0] = val&0x3F;
                chanLenCounter[0] = (64-chanLen[0])*clockSpeed/256;
                if (chanUseLen[0])
                    setSoundEventCycles(chanLenCounter[0]);
                sharedPtr->chanDuty[0] = val>>6;
                refreshSoundDuty(0);
                sendUpdateMessage(0);
                break;
            }
            // Envelope / Volume
        case 0x12:
            chanVol[0] = val>>4;
            if (val & 0x8)
                chanEnvDir[0] = 1;
            else
                chanEnvDir[0] = -1;
            chanEnvSweep[0] = val&0x7;
            refreshSoundVolume(0, true);
            break;
            // Frequency (low)
        case 0x13:
            chanFreq[0] &= 0x700;
            chanFreq[0] |= val;
            refreshSoundFreq(0);
            sendUpdateMessage(0);
            break;
            // Start / Frequency (high)
        case 0x14:
            chanFreq[0] &= 0xFF;
            chanFreq[0] |= (val&0x7)<<8;
            refreshSoundFreq(0);

            if (val & 0x40)
                chanUseLen[0] = 1;
            else
                chanUseLen[0] = 0;

            if (val & 0x80)
            {
                chanLenCounter[0] = (64-chanLen[0])*clockSpeed/256;
                if (chanUseLen[0])
                    setSoundEventCycles(chanLenCounter[0]);

                sharedPtr->chanOn |= CHAN_1;
                chanVol[0] = gameboy->readIO(0x12)>>4;
                if (chan1SweepTime != 0) {
                    chan1SweepCounter = clockSpeed/(128/chan1SweepTime);
                    setSoundEventCycles(chan1SweepCounter);
                }

                refreshSoundVolume(0, false);
                sendStartMessage(0);
            }
            else
                sendUpdateMessage(0);
            break;
            // CHANNEL 2
            // Length / Duty
        case 0x16:
            chanLen[1] = val&0x3F;
            chanLenCounter[1] = (64-chanLen[1])*clockSpeed/256;
            if (chanUseLen[1])
                setSoundEventCycles(chanLenCounter[1]);
            sharedPtr->chanDuty[1] = val>>6;
            sendUpdateMessage(1);
            break;
            // Volume / Envelope
        case 0x17:
            chanVol[1] = val>>4;
            if (val & 0x8)
                chanEnvDir[1] = 1;
            else
                chanEnvDir[1] = -1;
            chanEnvSweep[1] = val&0x7;
            refreshSoundVolume(1, true);
            break;
            // Frequency (low)
        case 0x18:
            chanFreq[1] &= 0x700;
            chanFreq[1] |= val;

            refreshSoundFreq(1);
            sendUpdateMessage(1);
            break;
            // Start / Frequency (high)
        case 0x19:
            chanFreq[1] &= 0xFF;
            chanFreq[1] |= (val&0x7)<<8;
            refreshSoundFreq(1);

            if (val & 0x40)
                chanUseLen[1] = 1;
            else
                chanUseLen[1] = 0;

            if (val & 0x80)
            {
                chanLenCounter[1] = (64-chanLen[1])*clockSpeed/256;
                if (chanUseLen[1])
                    setSoundEventCycles(chanLenCounter[1]);
                sharedPtr->chanOn |= CHAN_2;
                chanVol[1] = gameboy->readIO(0x17)>>4;

                refreshSoundVolume(1, false);
                sendStartMessage(1);
            }
            else
                sendUpdateMessage(1);
            break;
            // CHANNEL 3
            // On/Off
        case 0x1A:
            if ((val & 0x80) == 0)
            {
                sharedPtr->chanOn &= ~CHAN_3;
                sendUpdateMessage(2);
            }
            break;
            // Length
        case 0x1B:
            chanLen[2] = val;
            chanLenCounter[2] = (256-chanLen[2])*clockSpeed/256;
            if (chanUseLen[2])
                setSoundEventCycles(chanLenCounter[2]);
            break;
            // Volume
        case 0x1C:
            {
                chanVol[2] = (val>>5)&3;
                switch(chanVol[2])
                {
                    case 0:
                        break;
                    case 1:
                        chanVol[2] = 15;
                        break;
                    case 2:
                        chanVol[2] = 15>>1;
                        break;
                    case 3:
                        chanVol[2] = 15>>2;
                        break;
                }
                refreshSoundVolume(2, true);
                break;
            }
            // Frequency (low)
        case 0x1D:
            chanFreq[2] &= 0xFF00;
            chanFreq[2] |= val;
            refreshSoundFreq(2);
            sendUpdateMessage(2);
            break;
            // Start / Frequency (high)
        case 0x1E:
            chanFreq[2] &= 0xFF;
            chanFreq[2] |= (val&7)<<8;
            refreshSoundFreq(2);

            if (val & 0x40)
                chanUseLen[2] = 1;
            else
                chanUseLen[2] = 0;

            if ((val & 0x80) && (gameboy->readIO(0x1a) & 0x80))
            {
                sharedPtr->chanOn |= CHAN_3;
                chanLenCounter[2] = (256-chanLen[2])*clockSpeed/256;
                if (chanUseLen[2])
                    setSoundEventCycles(chanLenCounter[2]);

                refreshSoundVolume(2, false);
                sendStartMessage(2);
            }
            else {
                sendUpdateMessage(2);
            }
            break;
            // CHANNEL 4
            // Length
        case 0x20:
            chanLen[3] = val&0x3F;
            chanLenCounter[3] = (64-chanLen[3])*clockSpeed/256;
            if (chanUseLen[3])
                setSoundEventCycles(chanLenCounter[3]);
            break;
            // Volume / Envelope
        case 0x21:
            chanVol[3] = val>>4;
            if (val & 0x8)
                chanEnvDir[3] = 1;
            else
                chanEnvDir[3] = -1;
            chanEnvSweep[3] = val&0x7;
            refreshSoundVolume(3, true);
            break;
            // Frequency
        case 0x22:
            chanFreq[3] = val>>4;
            chan4FreqRatio = val&0x7;
            if (chan4FreqRatio == 0)
                chan4FreqRatio = 0.5;
            sharedPtr->lfsr7Bit = val&0x8;
            refreshSoundFreq(3);
            sendUpdateMessage(3);
            break;
            // Start
        case 0x23:
            chanUseLen[3] = !!(val&0x40);
            if (val&0x80)
            {
                chanLenCounter[3] = (64-chanLen[3])*clockSpeed/256;
                if (chanUseLen[3])
                    setSoundEventCycles(chanLenCounter[3]);
                sharedPtr->chanOn |= CHAN_4;
                chanVol[3] = gameboy->readIO(0x21)>>4;
                refreshSoundVolume(3, false);
                sendStartMessage(3);
            }
            break;
            // GENERAL REGISTERS
        case 0x24:
            if ((sharedPtr->volControl&0x7) != (val&0x7)) {
                sharedPtr->volControl = val;
                sendGlobalVolumeMessage();
            }
            else
                sharedPtr->volControl = val;
            break;
        case 0x25:
            sharedPtr->chanOutput = val;
            for (int i=0; i<4; i++)
                refreshSoundPan(i);
            sendUpdateMessage(-1);
            sendGlobalVolumeMessage();
            break;
        case 0x26:
            if (!(val&0x80))
            {
                sharedPtr->chanOn = 0;
                
                sendUpdateMessage(-1);
            }
            break;
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35:
        case 0x36:
        case 0x37:
        case 0x38:
        case 0x39:
        case 0x3A:
        case 0x3B:
        case 0x3C:
        case 0x3D:
        case 0x3E:
        case 0x3F:
            updateSoundSample(ioReg-0x30);
            break;
        default:
            break;
    }
}

void SoundEngine::updateSoundSample(int byte) {
    u8 sample = gameboy->readIO(0x30+byte);
    sampleData[byte*2] = pcmVals[sample>>4];
    sampleData[byte*2+1] = pcmVals[sample&0xf];
}


// If Sound Fix is enabled, enter a loop until exactly the right moment at which 
// the sound should be updated.
// NOTE: scale transfer, which is done by arm7, tends to interfere with this.
void SoundEngine::synchronizeSound() {
    if (muted)
        return;

    int cycles = gameboy->getCyclesSinceVBlank();
    if (gameboy->isDoubleSpeed())
        cycles /= 2;

    if (sharedPtr->hyperSound &&
            (!sharedPtr->scaleTransferReady) && // Scale transfer eats up a lot of arm7's time
            !(sharedPtr->frameFlip_Gameboy != sharedPtr->frameFlip_DS || sharedPtr->dsCycles >= cycles)) {

        sharedPtr->cycles = cycles;
        while (sharedPtr->cycles != -1) { // Wait for arm7 to set sharedPtr->cycles.
            if (sharedPtr->scaleTransferReady) { // Is arm7 doing the scale transfer? If so ABORT
                sharedPtr->cycles = -1;
                goto sendByFifo;
            }
        }
    }
    else {
sendByFifo:
        FIFO_SEND(sharedPtr->message);
    }
}

void SoundEngine::sendStartMessage(int i) {
    sharedPtr->message = GBSND_START_COMMAND<<20 | i;
    synchronizeSound();
}

void SoundEngine::sendUpdateMessage(int i) {
    if (i == -1)
        i = 4;
    sharedPtr->message = GBSND_UPDATE_COMMAND<<20 | i;
    synchronizeSound();
}

void SoundEngine::sendGlobalVolumeMessage() {
    sharedPtr->message = GBSND_MASTER_VOLUME_COMMAND<<20;
    synchronizeSound();
}



void SoundEngine::refreshSoundPan(int i) {
    if ((sharedPtr->chanOutput & (1<<i)) && (sharedPtr->chanOutput & (1<<(i+4))))
        sharedPtr->chanPan[i] = 64;
    else if (sharedPtr->chanOutput & (1<<i))
        sharedPtr->chanPan[i] = 127;
    else if (sharedPtr->chanOutput & (1<<(i+4)))
        sharedPtr->chanPan[i] = 0;
    else {
        sharedPtr->chanPan[i] = 128;   // Special signal
    }
}

void SoundEngine::refreshSoundVolume(int i, bool send)
{
    if (!(sharedPtr->chanOn & (1<<i)) || !sharedPtr->chanEnabled[i])
    {
        return;
    }

    int volume = chanVol[i];

    if (send && sharedPtr->chanRealVol[i] != volume) {
        sharedPtr->chanRealVol[i] = volume;
        sharedPtr->message = GBSND_VOLUME_COMMAND<<20 | i;
        synchronizeSound();
    }
    else
        sharedPtr->chanRealVol[i] = volume;
}

void SoundEngine::refreshSoundFreq(int i) {
    int freq=0;
    if (i <= 1) {
        freq = 131072/(2048-chanFreq[i])*8;
    }
    else if (i == 2) {
        freq = 65536/(2048-chanFreq[i])*32;
    }
    else if (i == 3) {
        freq = (int)(524288 / chan4FreqRatio) >> (chanFreq[i]+1);
        //freq = 0xaaa;
        //printLog("%.2x: Freq %x\n", ioRam[0x22], freq);
    }
    sharedPtr->chanRealFreq[i] = freq;
}

void SoundEngine::refreshSoundDuty(int i) {
    if ((sharedPtr->chanOn & (1<<i))) {
    }
}


// Global functions

void muteSND() {
    sharedData->fifosSent++;
    fifoSendValue32(FIFO_USER_01, GBSND_MUTE_COMMAND<<20);
}
void unmuteSND() {
    sharedData->fifosSent++;
    fifoSendValue32(FIFO_USER_01, GBSND_UNMUTE_COMMAND<<20);
}

void enableChannel(int i) {
    sharedData->chanEnabled[i] = true;
}
void disableChannel(int i) {
    sharedData->chanEnabled[i] = false;
}

