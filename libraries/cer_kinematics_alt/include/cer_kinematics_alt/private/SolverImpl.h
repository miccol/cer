/*
 * Copyright (C) 2015 iCub Facility - Istituto Italiano di Tecnologia
 * Author: Alessandro Scalzo
 * email:  alessandro.scalzo@iit.it
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

//#include <stdio.h>
//#include <time.h>
//#include <iostream>

#ifndef SOLVER_IMPL___
#define SOLVER_IMPL___

#include <yarp/os/Time.h>

#include <cer_kinematics_alt/private/Matrix.h>
#include <cer_kinematics_alt/private/Joints.h>

#include <cer_kinematics_alt/Solver.h>

#define TORSO_MAX_TILT          30.0  // [deg]
#define MIN_TORSO_EXTENSION    -0.05  // [m]
#define MAX_TORSO_EXTENSION     0.15  // [m]

#define MIN_ARM_EXTENSION       0.0   // [m]
#define MAX_ARM_EXTENSION       0.14  // [m]
#define WRIST_MAX_TILT          35.0  // [deg]

namespace cer {
namespace kinematics_alt {

#define NJOINTS 22

#define ROOT NULL

enum { R = 0, L = 1 };

class LeftSideSolverImpl
{
public:

    LeftSideSolverImpl();

    virtual ~LeftSideSolverImpl(void)
    {
        if (mRoot) delete mRoot;
    }

    Vec3 getCOM(){ return COM_valid; }

    void setPositionThreshold(double thr=DEFAULT_POS_THRESHOLD)
    {
        mPosThreshold=thr;
    }

    bool fkin(const yarp::sig::Vector &qin, yarp::sig::Matrix &H,int frame=-1)
    {
        if (frame<0) frame=L_WRIST_TRIPOD_3;

        if (frame>NJOINTS) return false;

        Matrix qconf(NJOINTS);

        int N=qin.length();

        if (N>NJOINTS) N=NJOINTS;

        for (int j=0; j<N; ++j) qconf(j)=qin(j);

        mRoot->calcPosture(qconf,T_ROOT);

        Rotation& R=mPart[frame]->Toj.Rj();
        Vec3& P=mPart[frame]->Toj.Pj();

        H.resize(4,4);
        H(0,0)=R(0,0); H(0,1)=R(0,1); H(0,2)=R(0,2); H(0,3)=P.x;
        H(1,0)=R(1,0); H(1,1)=R(1,1); H(1,2)=R(1,2); H(1,3)=P.y;
        H(2,0)=R(2,0); H(2,1)=R(2,1); H(2,2)=R(2,2); H(2,3)=P.z;
        H(3,0)=0.0;    H(3,1)=0.0;    H(3,2)=0.0;    H(3,3)=1.0;

        return true;
    }

    bool ikin(const yarp::sig::Matrix &Hd,
              yarp::sig::Vector &qout,
              double armElong=DEFAULT_ARM_EXTENSION,
              double torsoElong=DEFAULT_TORSO_EXTENSION,
              double timeoutSec=SOLVER_TIMEOUT)
    {
        if (torsoElong!=mTorsoExt)
        {
            double qa=torsoElong-0.5*mTorsoExc;
            double qb=torsoElong+0.5*mTorsoExc;

            if (qa<MIN_TORSO_EXTENSION){ qa=MIN_TORSO_EXTENSION; qb+=MIN_TORSO_EXTENSION-qa; }
            if (qb>MAX_TORSO_EXTENSION){ qa+=MAX_TORSO_EXTENSION-qb; qb=MAX_TORSO_EXTENSION; }

            mTorsoExt=0.5*(qa+qb);

            qzero(0)=qzero(1)=qzero(2)=mTorsoExt;
            qmin[0]=qmin[1]=qmin[2]=qa;
            qmax[0]=qmax[1]=qmax[2]=qb;
        }

        if (armElong!=mArmExt[L])
        {
            double qa=armElong-0.5*mArmExc[L];
            double qb=armElong+0.5*mArmExc[L];

            if (qa<MIN_ARM_EXTENSION){ qa=MIN_ARM_EXTENSION; qb+=MIN_ARM_EXTENSION-qa; }
            if (qb>MAX_ARM_EXTENSION){ qa+=MAX_ARM_EXTENSION-qb; qb=MAX_ARM_EXTENSION; }

            mArmExt[L]=0.5*(qa+qb);

            qzero(9)=qzero(10)=qzero(11)=mArmExt[L];
            qmin[9]=qmin[10]=qmin[11]=qa;
            qmax[9]=qmax[10]=qmax[11]=qb;
        }

        QtargetL=Rotation(Vec3(Hd(0,0),Hd(1,0),Hd(2,0)),Vec3(Hd(0,1),Hd(1,1),Hd(2,1)),Vec3(Hd(0,2),Hd(1,2),Hd(2,2))).quaternion();

        XtargetL=Vec3(Hd(0,3),Hd(1,3),Hd(2,3));

        q=q_valid=qzero;

        int ret_code=IN_PROGRESS;

        int steps=0;

        int N=qout.length();

        if (N>NJOINTS) N=NJOINTS;

        //leftHand2Target(XtargetL,QtargetL);
        //for (int j=0; j<N; ++j) qout(j)=q(j);

        double time_end=yarp::os::Time::now()+timeoutSec;

        while (yarp::os::Time::now()<=time_end)
        {
            ret_code=leftHand2Target(XtargetL,QtargetL);
        }
        
        if (ret_code==ON_TARGET)
        {
            for (int j=0; j<N; ++j) qout(j)=q(j);

            return true;
        }

        if (ret_code==IN_PROGRESS)
        {
            for (int j=0; j<N; ++j) qout(j)=q(j);

            return false;
        }

        return false;
    }
    /*
    bool controller(yarp::sig::Vector &_qposin, yarp::sig::Vector &_Vstar, yarp::sig::Vector &_Wstar, yarp::sig::Vector &qvelout)
    {
        mRoot->calcPosture(q,T_ROOT);

        mHand[L]->getJ(Jhand);

        J=Jhand.sub(0,6,0,12);

        Jv=J.sub(0,3,0,12);
        Jw=J.sub(3,3,0,12);

        ////////////////////////

        for (int j=0; j<12; ++j)
        {
            double s=(q(j)-qzero(j))/((q(j)>qzero(j))?(qmax[j]-qzero(j)):(qmin[j]-qzero(j)));

            if (s>1.0) s=1.0;

            A2(j,j)=(1.0-s*s)*W2(j,j);

            qz(j)=Zq[j]*(qzero(j)-q(j));
        }

        W2Jt=fast_mul_diag_full(W2,J.t());
        qz-=W2Jt*((J*W2Jt).inv()*(J*qz));

        ////////////////////////////////////////

            A2Jvt=fast_mul_diag_full(A2,Jv.t());
            (Jv*A2Jvt).base(lv,Rv);

            Lvi(0,0)=1.0/lv(0);
            Lvi(1,1)=1.0/lv(1);
            Lvi(2,2)=1.0/lv(2);

            Vrot=Lvi*Rv.t()*Vref;

            static const double VLIM=10.0;

            if (fabs(Vrot(0))>VLIM) Vrot(0)=(Vrot(0)>0.0?VLIM:-VLIM);
            if (fabs(Vrot(1))>VLIM) Vrot(1)=(Vrot(1)>0.0?VLIM:-VLIM);
            if (fabs(Vrot(2))>VLIM) Vrot(2)=(Vrot(2)>0.0?VLIM:-VLIM);

            qv=A2Jvt*(Rv*Vrot);
            Wref-=Jw*qv;

            /////////////////////////////////////////////////////////

            static const Matrix I=Matrix::id(12);

            W2Jvt=fast_mul_diag_full(A2,Jv.t());

            Nv=I-W2Jvt*(Jv*W2Jvt).inv()*Jv;

            Kw=Jw*Nv;

            /////////////////////////////////////////////////////////

            A2Kwt=fast_mul_diag_full(A2,Kw.t());
            (Jw*A2Kwt).base(lw,Rw);

            Lwi(0,0)=1.0/lw(0);
            Lwi(1,1)=1.0/lw(1);
            Lwi(2,2)=1.0/lw(2);

            Wrot=Lwi*Rw.t()*Wref;

            static const double WLIM=2.0;

            if (fabs(Wrot(0))>WLIM) Wrot(0)=(Wrot(0)>0.0?WLIM:-WLIM);
            if (fabs(Wrot(1))>WLIM) Wrot(1)=(Wrot(1)>0.0?WLIM:-WLIM);
            if (fabs(Wrot(2))>WLIM) Wrot(2)=(Wrot(2)>0.0?WLIM:-WLIM);

            qv+=Nv*(A2Kwt*(Rw*Wrot));

	    ////////////////////////////////////////

        for (int j=0; j<12; ++j) qvelout(j)=qv(j)+qz(j);

        limitJointSpeeds(q,qvelout);

        for (int j=0; j<12; ++j) qvelout(j)*=Kq[j];

        return true;
    }
    */
protected:

    void calcPostureFrom(Component* pComp,Transform& Tref)
    {
        pComp->calcPosture(q,Tref,NULL);
    }

    void calcPostureFromRoot(Transform& Tref)
    {
        calcPostureFrom(mRoot,Tref);
    }

    void limitJointAngles(Matrix& ql)
    {
        for (int j=0; j<NJOINTS; ++j)
        {
            if (ql(j)<=qmin[j])
            {
                ql(j)=qmin[j];
            }
            else if (ql(j)>=qmax[j])
            {
                ql(j)=qmax[j];
            }
        }
    }

    void limitJointSpeeds(Matrix& ql,Matrix& qd)
    {
        for (int j=0; j<NJOINTS; ++j)
        {
            if (qd(j)<-Q[j]) qd(j)=-Q[j]; else if (qd(j)>Q[j]) qd(j)=Q[j];
             
            if (ql(j)<=qmin[j])
            {
                if (qd(j)<0.0) qd(j)=0.0;
            }
            else if (ql(j)>=qmax[j])
            {
                if (qd(j)>0.0) qd(j)=0.0;
            }
        }
    }

    int checkG();

    int leftHand2Target(Vec3& Xstar,Quaternion& Qstar);
    int leftHandController(Vec3& Xstar,Quaternion& Qstar);

    Matrix q;
    Matrix qzero;
    Matrix q_valid;
    Vec3 COM;
    Vec3 COM_valid;

    Matrix Jg;
    Vec3 Gforce;

    Matrix W2;
    Matrix Jhand;
    Matrix J;
    Matrix Jv;
    Matrix Jw;
    Matrix A2;

    Matrix Vref,Wref;
    Matrix qz,qv;
    Matrix W2Jt;
    Matrix lv,Rv,Lvi,A2Jvt;
    Matrix Vrot;
    Matrix W2Jvt;
    Matrix Nv;
    Matrix Kw;
    Matrix lw,Rw,Lwi,A2Kwt;
    Matrix Wrot;

    Vec3 XtargetL,XtargetR;
    Quaternion QtargetL,QtargetR;

    int startNeck;
    int startLArm;
    int startRArm;
    int startLForeArm;
    int startRForeArm;

    double mTorsoExt,mTorsoExc;
    double mArmExt[2],mArmExc[2];

    int nj;

    Vec3 G[9];

    double qmin[NJOINTS],qmax[NJOINTS];
    double qp_old[NJOINTS];
    double Kz[NJOINTS];
    double Ks[NJOINTS];
    double Kq[NJOINTS];
    double Zq[NJOINTS];

    double Q[NJOINTS];

    double mPosThreshold;

    Component *mRoot;
    //Component *mBase;
    Component *mTorsoYaw;
    Component *mLShoulder;
    Component *mRShoulder;
    Component *mHand[2];

    Component *mUArm[2];
    Component *mFArm[2];
    Component *mHead;
    Component *mNeck;

    Component *mLshoulder0;
    Component *mRshoulder0;

    Component *mLshoulder1;
    Component *mRshoulder1;

    Component *mPart[NJOINTS];

    Joint *mJoint[NJOINTS];
    Trifid *mTrifidTorso;
    Trifid *mTrifidHand[2];

    Transform T_ROOT;

    enum { IMPOSSIBLE=-2,FAILURE=-1,IN_PROGRESS=0,ON_TARGET=1,OUT_OF_LIMITS=2,OUT_OF_REACH=3,FALLEN=4,BALANCED=5,FRONTIER=6 };
};

}
}

#endif