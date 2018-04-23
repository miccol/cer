/*
 * Copyright (C) 2018 iCub Facility - Istituto Italiano di Tecnologia
 * Authors: Alberto Cardellino <alberto.cardellino@iit.it>
 *          Francesco Diotalevi <francesco.diotalevi@iit.it>
 *
 * CopyPolicy: Released under the terms of the LGPLv2.1 or later, see LGPL.TXT
 */

#include <atomic>

#include "CircularBuffer.h"
#include <yarp/sig/Sound.h>
#include <yarp/os/RateThread.h>
#include <yarp/dev/DeviceDriver.h>
#include <yarp/dev/ServiceInterfaces.h>
#include <yarp/dev/AudioGrabberInterfaces.h>

namespace cer{
    namespace dev{
        class R1faceMic;
    }
}



template <class T> class CircularBuffer;
struct inputData;

/**
 *
 * \section R1faceMic
 *
 *  Description of input parameters
 *
 *  This device aquires audio channel from the robot and streams them through the YARP network via the 'grabber' wrapper
 *
 * Parameters accepted in the config argument of the open method:
 * | Parameter name | Type    | Units          | Default Value | Required  | Description   | Notes |
 * |:--------------:|:------: |:--------------:|:-------------:|:--------: |:-------------:|:-----:|
 * |  ????          | string  |  -             |   -           | Yes       |               |       |
 * |  ????          | string  |  -             | /dev/auxdisp  | Yes       |               |       |
 *
 *
 */

class cer::dev::R1faceMic:      public yarp::os::RateThread,
                                public yarp::dev::IService,
                                public yarp::dev::DeviceDriver,
                                public yarp::dev::IAudioGrabberSound
{
public:
    // Constructor used by yarp factory
    R1faceMic();

    ~R1faceMic();

    // Device driver interface
    bool open(yarp::os::Searchable &params);
    bool close();

    // Thread interface
    virtual bool threadInit();

    virtual void run();

    virtual void threadRelease();

    // IService interface

    virtual bool startService();

    virtual bool updateService();

    virtual bool stopService();

    // IAudioGrabberSound interface
    /**
     * Get a sound from a device.
     *
     * @param sound the sound to be filled
     * @return true/false upon success/failure
     */
    virtual bool getSound(yarp::sig::Sound& sound);

    /**
     * Start the recording.
     *
     * @return true/false upon success/failure
     */
    virtual bool startRecording();

     /**
     * Stop the recording.
     *
     * @return true/false upon success/failure
     */
    virtual bool stopRecording();

private:

    int  shift;
    bool recording;
    int  channels;
    int  samplingRate;
    int  dev_fd;
    int  chunkSize;
    std::string deviceFile;

    int32_t                     *rawBuffer;
    inputData                   *tmpData;
    CircularBuffer<inputData>   *inputBuffer;
};