#ifndef H_SOCKETCAN_DRIVER
#define H_SOCKETCAN_DRIVER

#include <socketcan_interface/asio_base.h>
#include <boost/bind.hpp>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/error.h>

#include <cstring>
#include <iostream>

#include <socketcan_interface/dispatcher.h>
#include <socketcan_interface/string.h>

namespace can {

class SocketCANInterface : public AsioDriver<boost::asio::posix::stream_descriptor> {
    bool loopback_;
    int sc_;
    can_err_mask_t error_mask_, fatal_error_mask_;

    static can_err_mask_t parse_error_mask(SettingsConstSharedPtr settings, const std::string &entry, can_err_mask_t defaults) {
        can_err_mask_t mask = 0;

        #define add_bit(e) mask |= (settings->get_optional(entry + "/" + #e, (defaults & e) != 0) ? e : 0)
        add_bit(CAN_ERR_LOSTARB);
        add_bit(CAN_ERR_CRTL);
        add_bit(CAN_ERR_PROT);
        add_bit(CAN_ERR_TRX);
        add_bit(CAN_ERR_ACK);
        add_bit(CAN_ERR_TX_TIMEOUT);
        add_bit(CAN_ERR_BUSOFF);
        add_bit(CAN_ERR_BUSERROR);
        add_bit(CAN_ERR_RESTARTED);
        #undef add_bit

        return mask;
    }
public:
    SocketCANInterface()
    : loopback_(false), sc_(-1), error_mask_(0), fatal_error_mask_(0)
    {}

    bool doesLoopBack() const override{
        return loopback_;
    }

    can_err_mask_t getErrorMask() const {
        return error_mask_;
    }

    can_err_mask_t getFatalErrorMask() const {
        return fatal_error_mask_;
    }
    [[deprecated("provide settings explicitly")]] virtual bool init(const std::string &device, bool loopback) override {
        return SocketCANInterface::init(device, loopback, NoSettings::create());
    }
    virtual bool init(const std::string &device, bool loopback, SettingsConstSharedPtr settings) override {
      if (!settings) {
          ROSCANOPEN_ERROR("socketcan_interface", "settings must not be a null pointer");
          return false;
      }
      const can_err_mask_t fatal_errors = ( CAN_ERR_TX_TIMEOUT   /* TX timeout (by netdevice driver) */
                                          | CAN_ERR_BUSOFF       /* bus off */
                                          | CAN_ERR_BUSERROR     /* bus error (may flood!) */
                                          | CAN_ERR_RESTARTED    /* controller restarted */
                                            );
      const can_err_mask_t report_errors = ( CAN_ERR_LOSTARB      /* lost arbitration    / data[0]    */
                                           | CAN_ERR_CRTL         /* controller problems / data[1]    */
                                           | CAN_ERR_PROT         /* protocol violations / data[2..3] */
                                           | CAN_ERR_TRX          /* transceiver status  / data[4]    */
                                           | CAN_ERR_ACK          /* received no ACK on transmission */
                                           );
      can_err_mask_t fatal_error_mask = parse_error_mask(settings, "fatal_error_mask", fatal_errors) | CAN_ERR_BUSOFF;
      can_err_mask_t error_mask = parse_error_mask(settings, "error_mask", report_errors | fatal_error_mask) | fatal_error_mask;
      return init(device, loopback, error_mask, fatal_error_mask);
    }

    bool recover() override{
        if(!getState().isReady()){
            shutdown();
            return init(device_, loopback_, error_mask_, fatal_error_mask_);
        }
        return getState().isReady();
    }
    bool translateError(unsigned int internal_error, std::string & str) override{

        bool ret = false;
        if(!internal_error){
            str = "OK";
            ret = true;
        }
        if( internal_error & CAN_ERR_TX_TIMEOUT){
            str += "TX timeout (by netdevice driver);";
            ret = true;
        }
        if( internal_error & CAN_ERR_LOSTARB){
            str += "lost arbitration;";
            ret = true;
        }
        if( internal_error & CAN_ERR_CRTL){
            str += "controller problems;";
            ret = true;
        }
        if( internal_error & CAN_ERR_PROT){
            str += "protocol violations;";
            ret = true;
        }
        if( internal_error & CAN_ERR_TRX){
            str += "transceiver status;";
            ret = true;
        }
        if( internal_error & CAN_ERR_BUSOFF){
            str += "bus off;";
            ret = true;
        }
        if( internal_error & CAN_ERR_RESTARTED){
            str += "controller restarted;";
            ret = true;
        }
        return ret;
    }
    int getInternalSocket() {
        return sc_;
    }
protected:
    std::string device_;
    can_frame frame_;

    bool init(const std::string &device, bool loopback, can_err_mask_t error_mask, can_err_mask_t fatal_error_mask) {
        State s = getState();
        if(s.driver_state == State::closed){
            sc_ = 0;
            device_ = device;
            loopback_ = loopback;
            error_mask_ = error_mask;
            fatal_error_mask_ = fatal_error_mask;

            int sc = socket( PF_CAN, SOCK_RAW, CAN_RAW );
            if(sc < 0){
                setErrorCode(boost::system::error_code(sc,boost::system::system_category()));
                return false;
            }

            struct ifreq ifr;
            strcpy(ifr.ifr_name, device_.c_str());
            int ret = ioctl(sc, SIOCGIFINDEX, &ifr);

            if(ret != 0){
                setErrorCode(boost::system::error_code(ret,boost::system::system_category()));
                close(sc);
                return false;
            }
            ret = setsockopt(sc, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
               &error_mask, sizeof(error_mask));

            if(ret != 0){
                setErrorCode(boost::system::error_code(ret,boost::system::system_category()));
                close(sc);
                return false;
            }

            if(loopback_){
                int recv_own_msgs = 1; /* 0 = disabled (default), 1 = enabled */
                ret = setsockopt(sc, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &recv_own_msgs, sizeof(recv_own_msgs));

                if(ret != 0){
                    setErrorCode(boost::system::error_code(ret,boost::system::system_category()));
                    close(sc);
                    return false;
                }
            }

            struct sockaddr_can addr = {0};
            addr.can_family = AF_CAN;
            addr.can_ifindex = ifr.ifr_ifindex;
            ret = bind( sc, (struct sockaddr*)&addr, sizeof(addr) );

            if(ret != 0){
                setErrorCode(boost::system::error_code(ret,boost::system::system_category()));
                close(sc);
                return false;
            }

            boost::system::error_code ec;
            socket_.assign(sc,ec);

            setErrorCode(ec);

            if(ec){
                close(sc);
                return false;
            }
            setInternalError(0);
            setDriverState(State::open);
            sc_ = sc;
            return true;
        }
        return getState().isReady();
    }

    void triggerReadSome() override{
        boost::mutex::scoped_lock lock(send_mutex_);
        socket_.async_read_some(boost::asio::buffer(&frame_, sizeof(frame_)), boost::bind( &SocketCANInterface::readFrame,this, boost::asio::placeholders::error));
    }

    bool enqueue(const Frame & msg) override{
        boost::mutex::scoped_lock lock(send_mutex_); //TODO: timed try lock

        can_frame frame = {0};
        frame.can_id = msg.id | (msg.is_extended?CAN_EFF_FLAG:0) | (msg.is_rtr?CAN_RTR_FLAG:0);;
        frame.can_dlc = msg.dlc;


        for(int i=0; i < frame.can_dlc;++i)
            frame.data[i] = msg.data[i];

        boost::system::error_code ec;
        boost::asio::write(socket_, boost::asio::buffer(&frame, sizeof(frame)),boost::asio::transfer_all(), ec);
        if(ec){
            ROSCANOPEN_ERROR("socketcan_interface", "FAILED " << ec);
            setErrorCode(ec);
            setNotReady();
            return false;
        }

        return true;
    }

    void readFrame(const boost::system::error_code& error){
        if(!error){
            input_.dlc = frame_.can_dlc;
            for(int i=0;i<frame_.can_dlc && i < 8; ++i){
                input_.data[i] = frame_.data[i];
            }

            if(frame_.can_id & CAN_ERR_FLAG){ // error message
                input_.id = frame_.can_id & CAN_EFF_MASK;
                input_.is_error = 1;
                if (frame_.can_id & fatal_error_mask_) {
                    std::cout << "FATAL read frame error detected, skipping internal error... frame id: " << +frame_.can_id  << std::endl;
                    //ROSCANOPEN_ERROR("socketcan_interface", "internal error: " << input_.id);
                    //setInternalError(input_.id);
                    //setNotReady();
                }
            }else{
                input_.is_extended = (frame_.can_id & CAN_EFF_FLAG) ? 1 :0;
                input_.id = frame_.can_id & (input_.is_extended ? CAN_EFF_MASK : CAN_SFF_MASK);
                input_.is_error = 0;
                input_.is_rtr = (frame_.can_id & CAN_RTR_FLAG) ? 1 : 0;
            }

        }
        frameReceived(error);
    }
private:
    boost::mutex send_mutex_;
};

using SocketCANDriver = SocketCANInterface;
using SocketCANDriverSharedPtr = std::shared_ptr<SocketCANDriver>;
using SocketCANInterfaceSharedPtr = std::shared_ptr<SocketCANInterface>;

template <typename T> class ThreadedInterface;
using ThreadedSocketCANInterface = ThreadedInterface<SocketCANInterface>;
using ThreadedSocketCANInterfaceSharedPtr = std::shared_ptr<ThreadedSocketCANInterface>;


} // namespace can
#endif
