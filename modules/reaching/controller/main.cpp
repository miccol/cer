/*
 * Copyright (C) 2015 iCub Facility - Istituto Italiano di Tecnologia
 * Author: Ugo Pattacini
 * email:  ugo.pattacini@iit.it
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

#include <string>
#include <algorithm>

#include <yarp/os/all.h>
#include <yarp/dev/all.h>
#include <yarp/sig/all.h>
#include <yarp/math/Math.h>

#include <iCub/ctrl/minJerkCtrl.h>
#include <cer_kinematics/arm.h>

#define MIN_TS  0.01    // [s]

using namespace std;
using namespace yarp::os;
using namespace yarp::dev;
using namespace yarp::sig;
using namespace yarp::math;
using namespace iCub::ctrl;
using namespace cer::kinematics;

// forward declaration
class Controller;

/****************************************************************/
class TargetPort : public BufferedPort<Property>
{
    Controller *ctrl;

    /****************************************************************/
    void onRead(Property &target);

public:
    /****************************************************************/
    TargetPort() : ctrl(NULL) { }

    /****************************************************************/
    void setController(Controller *ctrl) { this->ctrl=ctrl; }
};


/****************************************************************/
class Controller : public RFModule
{
    PolyDriver         drivers[4];
    VectorOf<int>      jointsIndexes[4];
    IControlMode2*     imod[4];
    IEncodersTimed*    ienc[4];
    IPositionControl2* ipos[4];
    IPositionDirect*   iposd[4];
    
    VectorOf<int> posDirectMode;
    VectorOf<int> curMode;

    ArmSolver solver;
    minJerkTrajGen* gen;
    
    BufferedPort<Vector> statePort;
    TargetPort targetPort;
    RpcServer rpcPort;
    RpcClient solverPort;
    Stamp txInfo;

    Mutex mutex;
    int verbosity;
    bool closing;
    bool controlling;
    double stop_threshold_revolute;
    double stop_threshold_prismatic;
    double Ts;
    Vector qd,xd;    

    /****************************************************************/
    Vector getEncoders(double *timeStamp=NULL)
    {
        Vector encs(12,0.0);
        Vector stamps(encs.length());

        Vector encs_=encs;
        Vector stamps_=stamps;

        ienc[0]->getEncodersTimed(&encs_[0],&stamps_[0]);
        encs.setSubvector(0,encs_.subVector(0,2));
        stamps.setSubvector(0,stamps_.subVector(0,2));

        ienc[1]->getEncodersTimed(&encs_[0],&stamps_[0]);
        encs[3]=encs_[3];
        stamps[3]=stamps_[3];

        ienc[2]->getEncodersTimed(&encs_[0],&stamps_[0]);
        encs.setSubvector(4,encs_.subVector(0,4));
        stamps.setSubvector(4,stamps_.subVector(0,4));

        ienc[3]->getEncodersTimed(&encs_[0],&stamps_[0]);
        encs.setSubvector(9,encs_.subVector(0,2));
        stamps.setSubvector(9,stamps_.subVector(0,2));

        if (timeStamp!=NULL)
            *timeStamp=findMax(stamps);

        return encs;
    }

    /****************************************************************/
    void getCurrentMode()
    {
        imod[0]->getControlModes(jointsIndexes[0].size(),jointsIndexes[0].getFirst(),&curMode[0]);
        imod[1]->getControlModes(jointsIndexes[1].size(),jointsIndexes[1].getFirst(),&curMode[3]);
        imod[2]->getControlModes(jointsIndexes[2].size(),jointsIndexes[2].getFirst(),&curMode[4]);
        imod[3]->getControlModes(jointsIndexes[3].size(),jointsIndexes[3].getFirst(),&curMode[9]);
    }

    /****************************************************************/
    bool areJointsHealthy()
    {
        for (size_t i=0; i<curMode.size(); i++)
            if ((curMode[i]==VOCAB_CM_HW_FAULT) || (curMode[i]==VOCAB_CM_IDLE))
                return false;
        return true;
    }

    /****************************************************************/
    bool setPositionDirectMode()
    {
        for (size_t i=0; i<3; i++)
        {
            if (curMode[i]!=posDirectMode[i])
            {
                imod[0]->setControlModes(jointsIndexes[0].size(),jointsIndexes[0].getFirst(),&posDirectMode[0]);
                break;
            }
        }

        for (size_t i=3; i<4; i++)
        {
            if (curMode[i]!=posDirectMode[i])
            {
                imod[1]->setControlModes(jointsIndexes[1].size(),jointsIndexes[1].getFirst(),&posDirectMode[3]);
                break;
            }
        }

        for (size_t i=4; i<9; i++)
        {
            if (curMode[i]!=posDirectMode[i])
            {
                imod[2]->setControlModes(jointsIndexes[2].size(),jointsIndexes[2].getFirst(),&posDirectMode[4]);
                break;
            }
        }

        for (size_t i=9; i<curMode.size(); i++)
        {
            if (curMode[i]!=posDirectMode[i])
            {
                imod[3]->setControlModes(jointsIndexes[3].size(),jointsIndexes[3].getFirst(),&posDirectMode[9]);
                break;
            }
        }

        return true;
    }

    /****************************************************************/
    void stopControl()
    {        
        for (int i=0; i<4; i++)
            ipos[i]->stop(jointsIndexes[i].size(),jointsIndexes[i].getFirst());
        controlling=false;
    }

    /****************************************************************/
    bool dist(const Vector &e)
    {
        Vector e_revolute=e.subVector(3,8);
        Vector e_prismatic=cat(e.subVector(0,2),e.subVector(9,11));
        return (norm(e_revolute)<stop_threshold_revolute) &&
               (norm(e_prismatic)<stop_threshold_prismatic);
    }

    /****************************************************************/
    Property prepareSolverOptions(const string &key, const Value &val)
    {
        Bottle b;
        Bottle &payLoad=b.addList().addList();
        payLoad.addString(key);
        payLoad.add(val);

        Property option;
        option.put("parameters",b.get(0));

        return option;
    }

    /****************************************************************/
    Value parseSolverOptions(const Bottle &rep, const string &key)
    {
        Value val;
        if (Bottle *payLoad=rep.find("parameters").asList())
            val=payLoad->find(key);

        return val;
    }

public:
    /****************************************************************/
    Controller() : gen(NULL)
    {
    }

    /****************************************************************/
    bool configure(ResourceFinder &rf)
    {
        string robot=rf.check("robot",Value("cer")).asString();
        string arm_type=rf.check("arm-type",Value("left")).asString();
        verbosity=rf.check("verbosity",Value(0)).asInt();
        stop_threshold_revolute=rf.check("stop-threshold-revolute",Value(2.0)).asDouble();
        stop_threshold_prismatic=rf.check("stop-threshold-prismatic",Value(0.002)).asDouble();
        double T=rf.check("T",Value(2.0)).asDouble();
        Ts=rf.check("Ts",Value(MIN_TS)).asDouble();
        Ts=std::max(Ts,MIN_TS);

        Property option;

        option.put("device","remote_controlboard");
        option.put("remote",("/"+robot+"/torso_tripod").c_str());
        option.put("local",("/cer_reaching-controller/"+arm_type+"/torso_tripod").c_str());
        option.put("writeStrict","on");
        if (!drivers[0].open(option))
        {
            yError("Unable to connect to %s",("/"+robot+"/torso_tripod").c_str());
            close();
            return false;
        }

        option.clear();
        option.put("device","remote_controlboard");
        option.put("remote",("/"+robot+"/torso").c_str());
        option.put("local",("/cer_reaching-controller/"+arm_type+"/torso").c_str());
        option.put("writeStrict","on");
        if (!drivers[1].open(option))
        {
            yError("Unable to connect to %s",("/"+robot+"/torso").c_str());
            close();
            return false;
        }

        option.clear();
        option.put("device","remote_controlboard");
        option.put("remote",("/"+robot+"/"+arm_type+"_arm").c_str());
        option.put("local",("/cer_reaching-controller/"+arm_type+"/"+arm_type+"_arm").c_str());
        option.put("writeStrict","on");
        if (!drivers[2].open(option))
        {
            yError("Unable to connect to %s",("/"+robot+"/"+arm_type+"_arm").c_str());
            close();
            return false;
        }

        option.clear();
        option.put("device","remote_controlboard");
        option.put("remote",("/"+robot+"/"+arm_type+"_wrist_tripod").c_str());
        option.put("local",("/cer_reaching-controller/"+arm_type+"/"+arm_type+"_wrist_tripod").c_str());
        option.put("writeStrict","on");
        if (!drivers[3].open(option))
        {
            yError("Unable to connect to %s",("/"+robot+"/"+arm_type+"_wrist_tripod").c_str());
            close();
            return false;
        }

        for (int i=0; i<4; i++)
        {
            drivers[i].view(imod[i]);
            drivers[i].view(ienc[i]);
            drivers[i].view(ipos[i]);
            drivers[i].view(iposd[i]);
        }

        // torso_tripod
        jointsIndexes[0].push_back(0);
        jointsIndexes[0].push_back(1);
        jointsIndexes[0].push_back(2);

        // torso (yaw)
        jointsIndexes[1].push_back(3);

        // arm
        jointsIndexes[2].push_back(0);
        jointsIndexes[2].push_back(1);
        jointsIndexes[2].push_back(2);
        jointsIndexes[2].push_back(3);
        jointsIndexes[2].push_back(4);

        // wrist_tripod
        jointsIndexes[3].push_back(0);
        jointsIndexes[3].push_back(1);
        jointsIndexes[3].push_back(2);

        statePort.open(("/cer_reaching-controller/"+arm_type+"/state:o").c_str());
        solverPort.open(("/cer_reaching-controller/"+arm_type+"/solver:rpc").c_str());

        targetPort.open(("/cer_reaching-controller/"+arm_type+"/target:i").c_str());
        targetPort.setController(this);
        targetPort.useCallback();

        rpcPort.open(("/cer_reaching-controller/"+arm_type+"/rpc").c_str());
        attach(rpcPort);

        qd=getEncoders();
        for (size_t i=0; i<qd.length(); i++)
            posDirectMode.push_back(VOCAB_CM_POSITION_DIRECT);
        curMode=posDirectMode;

        getCurrentMode();
        
        ArmParameters arm(arm_type);
        arm.upper_arm.setAllConstraints(false);
        solver.setArmParameters(arm);

        gen=new minJerkTrajGen(qd,Ts,T);
        closing=controlling=false;        

        return true;
    }

    /****************************************************************/
    bool close()
    {
        closing=true;

        if (controlling)
            stopControl();

        if (!targetPort.isClosed())
            targetPort.close(); 

        if (!statePort.isClosed())
            statePort.close();

        if (!solverPort.asPort().isOpen())
            solverPort.close();

        if (!rpcPort.asPort().isOpen())
            rpcPort.close();
                
        for (int i=0; i<4; i++)
            if (drivers[i].isValid())
                drivers[i].close(); 

        delete gen;
        return true;
    }

    /****************************************************************/
    double getPeriod()
    {
        return Ts;
    }

    /****************************************************************/
    bool go(Property &request)
    {
        if (closing)
            return false;

        if (verbosity>0)
            yInfo("Received request: %s",request.toString().c_str());

        if (request.check("stop"))
        {
            LockGuard lg(mutex);
            stopControl();
            return true;
        }

        if (!request.check("q"))
        {
            Vector q=getEncoders();
            Bottle b; b.addList().read(q);
            request.put("q",b.get(0));
        }

        if (verbosity>0)
            yInfo("Forwarding request to solver: %s",request.toString().c_str());

        Bottle reply;
        bool latch_controlling=controlling;
        if (solverPort.write(request,reply))
        {
            if (verbosity>0)
                yInfo("Received reply from solver: %s",reply.toString().c_str());

            if (reply.get(0).asVocab()==Vocab::encode("ack"))
            {
                if ((reply.size()>1) && !reply.check("parameters"))
                {
                    Bottle *payLoadJoints=reply.find("q").asList();
                    Bottle *payLoadPose=reply.find("x").asList();

                    if ((payLoadJoints!=NULL) && (payLoadPose!=NULL))
                    {
                        LockGuard lg(mutex);
                        // process only if we didn't receive
                        // a stop request in the meanwhile
                        if (controlling==latch_controlling)
                        {
                            for (size_t i=0; i<qd.length(); i++)
                                qd[i]=payLoadJoints->get(i).asDouble();

                            if (xd.length()==0)
                                xd.resize((size_t)payLoadPose->size());

                            for (size_t i=0; i<xd.length(); i++)
                                xd[i]=payLoadPose->get(i).asDouble();

                            if (!controlling)
                                gen->init(getEncoders());

                            controlling=true;
                            if (verbosity>0)
                                yInfo("Going to: %s",qd.toString(3,3).c_str());
                        }
                    }
                }

                return true;
            }
            else
                yError("Malformed target type!");
        }
        else
            yError("Unable to communicate with the solver");

        return false;
    }

    /****************************************************************/
    bool updateModule()
    {
        LockGuard lg(mutex);
        getCurrentMode();        

        Matrix Hee;
        double timeStamp;
        Vector q=getEncoders(&timeStamp);
        solver.fkin(q,Hee);

        Vector &pose=statePort.prepare();
        pose=Hee.getCol(3).subVector(0,2);

        Vector oee=dcm2axis(Hee);
        pose=cat(pose,oee);

        if (timeStamp>=0.0)
            txInfo.update(timeStamp);
        else
            txInfo.update();

        statePort.setEnvelope(txInfo);
        statePort.writeStrict();

        if (controlling)
        {
            gen->computeNextValues(qd);
            Vector ref=gen->getPos();

            if (verbosity>1)
                yInfo("Commanding new set-points: %s",ref.toString(3,3).c_str());

            if (areJointsHealthy())
            {
                setPositionDirectMode(); 
                iposd[0]->setPositions(jointsIndexes[0].size(),jointsIndexes[0].getFirst(),&ref[0]);
                iposd[1]->setPositions(jointsIndexes[1].size(),jointsIndexes[1].getFirst(),&ref[3]);
                iposd[2]->setPositions(jointsIndexes[2].size(),jointsIndexes[2].getFirst(),&ref[4]);
                iposd[3]->setPositions(jointsIndexes[3].size(),jointsIndexes[3].getFirst(),&ref[9]);

                if (dist(qd-q))
                {
                    controlling=false;
                    if (verbosity>0)
                        yInfo("Just stopped at: %s",q.toString(3,3).c_str());
                }
            }
            else
            {
                yWarning("Detected joints in HW_FAULT and/or IDLE => stopping control");
                stopControl();                
            }
        }

        return true;
    }

    /****************************************************************/
    bool respond(const Bottle &cmd, Bottle &reply)
    {
        int cmd_0=cmd.get(0).asVocab();
        if (cmd.size()==3)
        {
            if (cmd_0==Vocab::encode("set"))
            {
                string cmd_1=cmd.get(1).asString();
                if (cmd_1=="T")
                {
                    mutex.lock();
                    gen->setT(cmd.get(2).asDouble());
                    mutex.unlock();

                    reply.addVocab(Vocab::encode("ack"));
                }
                else if (cmd_1=="Ts")
                {
                    Ts=cmd.get(2).asDouble();
                    Ts=std::max(Ts,MIN_TS);

                    mutex.lock();
                    gen->setTs(Ts);
                    mutex.unlock();

                    reply.addVocab(Vocab::encode("ack"));
                }
                else if (cmd_1=="verbosity")
                {
                    mutex.lock();
                    verbosity=cmd.get(2).asInt();
                    mutex.unlock();

                    reply.addVocab(Vocab::encode("ack"));
                }
                else if (cmd_1=="mode")
                {
                    Value mode(cmd.get(2).asString());
                    Property p=prepareSolverOptions("mode",mode);

                    if (go(p))
                        reply.addVocab(Vocab::encode("ack"));
                }
                else if (cmd_1=="torso_heave")
                {
                    Value torso_heave(cmd.get(2).asDouble());
                    Property p=prepareSolverOptions("torso_heave",torso_heave);

                    if (go(p))
                        reply.addVocab(Vocab::encode("ack"));
                }
                else if (cmd_1=="lower_arm_heave")
                {
                    Value lower_arm_heave(cmd.get(2).asDouble());
                    Property p=prepareSolverOptions("lower_arm_heave",lower_arm_heave);

                    if (go(p))
                        reply.addVocab(Vocab::encode("ack"));
                }
                else if (cmd_1=="tol")
                {
                    Value tol(cmd.get(2).asDouble());
                    Property p=prepareSolverOptions("tol",tol);

                    if (go(p))
                        reply.addVocab(Vocab::encode("ack"));
                }
                else if (cmd_1=="constr_tol")
                {
                    Value constr_tol(cmd.get(2).asDouble());
                    Property p=prepareSolverOptions("constr_tol",constr_tol);

                    if (go(p))
                        reply.addVocab(Vocab::encode("ack"));
                }
            }
        }
        else if (cmd.size()==2)
        {
            if (cmd_0==Vocab::encode("get"))
            {
                string cmd_1=cmd.get(1).asString();
                if (cmd_1=="done")
                {
                    reply.addVocab(Vocab::encode("ack"));

                    mutex.lock();
                    reply.addInt(controlling?0:1);
                    mutex.unlock();
                }
                else if (cmd_1=="target")
                {
                    reply.addVocab(Vocab::encode("ack"));

                    mutex.lock();
                    reply.addList().read(xd);
                    mutex.unlock();
                }
                else if (cmd_1=="T")
                {
                    reply.addVocab(Vocab::encode("ack"));

                    mutex.lock();
                    reply.addDouble(gen->getT());
                    mutex.unlock();
                }
                else if (cmd_1=="Ts")
                {
                    reply.addVocab(Vocab::encode("ack"));

                    mutex.lock();
                    reply.addDouble(Ts);
                    mutex.unlock();
                }
                else if (cmd_1=="verbosity")
                {
                    reply.addVocab(Vocab::encode("ack"));

                    mutex.lock();
                    reply.addInt(verbosity);
                    mutex.unlock();
                }
                else if (cmd_1=="mode")
                {
                    Bottle req,rep;
                    req.addList().addString("get");
                    if (solverPort.write(req,rep))
                    {
                        Value mode=parseSolverOptions(rep,"mode");
                        reply.addVocab(Vocab::encode("ack"));
                        reply.add(mode);
                    }
                }
                else if (cmd_1=="torso_heave")
                {
                    Bottle req,rep;
                    req.addList().addString("get");
                    if (solverPort.write(req,rep))
                    {
                        Value torso_heave=parseSolverOptions(rep,"torso_heave");
                        reply.addVocab(Vocab::encode("ack"));
                        reply.add(torso_heave);
                    }
                }
                else if (cmd_1=="lower_arm_heave")
                {
                    Bottle req,rep;
                    req.addList().addString("get");
                    if (solverPort.write(req,rep))
                    {
                        Value lower_arm_heave=parseSolverOptions(rep,"lower_arm_heave");
                        reply.addVocab(Vocab::encode("ack"));
                        reply.add(lower_arm_heave);
                    }
                }
                else if (cmd_1=="tol")
                {
                    Bottle req,rep;
                    req.addList().addString("get");
                    if (solverPort.write(req,rep))
                    {
                        Value tol=parseSolverOptions(rep,"tol");
                        reply.addVocab(Vocab::encode("ack"));
                        reply.add(tol);
                    }
                }
                else if (cmd_1=="constr_tol")
                {
                    Bottle req,rep;
                    req.addList().addString("get");
                    if (solverPort.write(req,rep))
                    {
                        Value constr_tol=parseSolverOptions(rep,"constr_tol");
                        reply.addVocab(Vocab::encode("ack"));
                        reply.add(constr_tol);
                    }
                }
            }
            else if (cmd_0==Vocab::encode("go"))
            {
                if (Bottle *b=cmd.get(1).asList())
                {
                    Property p(b->toString().c_str());
                    if (go(p))
                        reply.addVocab(Vocab::encode("ack"));
                }
            }
        }
        else if (cmd_0==Vocab::encode("stop"))
        {
            mutex.lock();
            stopControl();
            mutex.unlock();

            reply.addVocab(Vocab::encode("ack"));
        }

        if (reply.size()==0)
            reply.addVocab(Vocab::encode("nack")); 
        
        return true;
    }
};


/****************************************************************/
void TargetPort::onRead(Property &target)
{
    if (ctrl!=NULL)
        ctrl->go(target);
}


/****************************************************************/
int main(int argc, char *argv[])
{
    Network yarp;
    if (!yarp.checkNetwork())
    {
        yError("YARP server not available!");
        return 1;
    }
    
    ResourceFinder rf;
    rf.configure(argc,argv);

    Controller controller;
    return controller.runModule(rf);
}

