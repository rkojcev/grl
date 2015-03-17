#ifndef _KUKA_VREP_INTERFACE_HPP_
#define _KUKA_VREP_INTERFACE_HPP_

#include <tuple>
#include <memory>
#include <thread>
#include <boost/asio.hpp>
#include <boost/circular_buffer.hpp>
#include "robone/KukaFRI.hpp"

namespace robone {

/// @brief the purpose of this class is to integrate the KukaFRI driver with the VRep interface, which requires its own thread to be maintained    
/// all async calls are done in a separate user thread
/// you must call the async calls then call run_user()
/// to call the corresponding handlers
/// @todo Either rename this to be FRI specific, or maybe have one FRIKuka class, one JavaKuka class, and one new class that should have the KukaVrep name combining the two.
/// @todo generalize this if possible to work for multiple devices beyond the kuka
//template<template <typename> Allocator = std::allocator>
class KukaVrep : std::enable_shared_from_this<KukaVrep> {
    
public:
    enum ParamIndex {
        localhost,  // 192.170.10.100
        localport,  // 30200
        remotehost, // 192.170.10.2
        remoteport  // 30200
    };
    
    /// @todo allow default params
    typedef std::tuple<std::string,std::string,std::string,std::string> Params;
    
    static const Params defaultParams(){
        return std::make_tuple(std::string("192.170.10.100"),std::string("30200"),std::string("192.170.10.2"),std::string("30200"));
    }
    
    /// @todo move this to params
    static const int default_circular_buffer_size = 3;
    
	KukaVrep(boost::asio::io_service& ios, Params params = defaultParams())
        :
        io_service_(ios),
        strand_(ios)
    {

        boost::asio::ip::udp::socket s(io_service_, boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string(std::get<localhost>(params)), boost::lexical_cast<short>(std::get<localport>(params))));

        boost::asio::ip::udp::resolver resolver(io_service_);
        boost::asio::ip::udp::endpoint endpoint = *resolver.resolve({boost::asio::ip::udp::v4(), std::get<remotehost>(params), std::get<remoteport>(params)});
    	s.connect(endpoint);
        
        iiwaP.reset(new robot::arm::kuka::iiwa(std::move(s)));
        
        
        for (int i = 0; i<default_circular_buffer_size; ++i) {
           monitorStates.push_back(std::make_shared<robot::arm::kuka::iiwa::MonitorState>());
   	       commandStates.push_back(std::make_shared<robot::arm::kuka::iiwa::CommandState>());
   	     }
    
        std::shared_ptr<robot::arm::kuka::iiwa::MonitorState> ms = monitorStates.front();
        monitorStates.pop_front();
        std::shared_ptr<robot::arm::kuka::iiwa::CommandState> cs = commandStates.front();
        commandStates.pop_front();
    
        auto self(shared_from_this());
        // start running the
        update_state(boost::system::error_code(),0,ms,cs,self);
        
        // start up the driver thread
        /// @todo perhaps allow user to control this?
        driver_threadP.reset(new std::thread([&]{ io_service_.run(); }));
	}
	
	~KukaVrep(){
		workP.reset();
	}
	
	void sendControlPointToJava(){
		
	}
	
    
    // todo
    /// pass a handler with the signature void f(std::shared_ptr<robot::arm::kuka::iiwa::MonitorState>)
    /// note that if no states are available, an empty sharedptr will be posted,
    /// so be sure to check if it is valid
    /// @todo instead of posting an empty shared ptr consider also posting the corresponding error code
    template<typename Handler>
	void async_getLatestState(Handler && handler){
        auto self(shared_from_this());
        
		io_service_.post(strand_.wrap(std::bind([this,self](Handler && handler){
		    // now in io_service_ thread, can get latest state
            std::shared_ptr<robot::arm::kuka::iiwa::MonitorState> monitorP;
            if(!monitorStates.empty()){
                monitorP = monitorStates.front();
                monitorStates.pop_front();
            }

            // don't need to wrap this one because it happens in the user thread
            user_io_service_.post(std::bind([monitorP](Handler && handler){
                // now in user_io_service_thread, can pass the final result to the user
                handler(monitorP);
            },std::move(handler)));
		},std::move(handler))));
	}
    
    /// @todo maybe allow a handler so the user can get their commands back?
    /// @todo change this to accept a unique_ptr (may be easier with C++14
    void async_sendCommand(std::shared_ptr<robot::arm::kuka::iiwa::CommandState> commandStateP){
        auto self (shared_from_this());
        
		io_service_.post(strand_.wrap([this,self, commandStateP](){
		    // now in io_service_ thread, can set latest state
            commandStates.push_front(commandStateP);
		}));
        
    }
    
  /// The io_service::run() call will block until all asynchronous operations
  /// have finished.
    /// @todo not implemented
    void run(){
    }
    
    /// run_user will only block briefly as all the requested callbacks that have been completed are made
    void run_user(){
        user_io_service_.run();
    }

private:

     /// @todo how to handle command state? If the user doesn't provide one what do we do? Copy the monitor state? This may be handled already in KukaFRI
    void update_state(boost::system::error_code ec, std::size_t bytes_transferred, std::shared_ptr<robot::arm::kuka::iiwa::MonitorState> ms, std::shared_ptr<robot::arm::kuka::iiwa::CommandState> cs, std::shared_ptr<KukaVrep> self){
        // new data available
        BOOST_VERIFY(monitorStates.size()>0); // we should always keep at least one state available

        // put the latest state on the front and get the oldest from the back
        monitorStates.push_front(ms);
        ms = monitorStates.back();
        monitorStates.pop_back();
        
        if(!commandStates.empty()){
            // this state is old, put it in back of the line
            commandStates.push_back(cs);
            // get the latest out front
            cs = commandStates.front();
            commandStates.pop_front();
        }
        
        if(!io_service_.stopped()) iiwaP->async_update(*ms,*cs,
                                                               //strand_.wrap( /// @todo FIXME
                                                                [](boost::system::error_code ec, std::size_t bytes_transferred){
                                                                // do nothing
                                                                }
                                                               //)
                                                      );
        // rerun the kuka API
//        if(!io_service_.stopped()) iiwaP->async_update(*ms,*cs,
//                                                               //strand_.wrap( /// @todo FIXME
//                                                                 std::bind(&KukaVrep::update_state,this,boost::asio::placeholders::error,boost::asio::placeholders::bytes_transferred,ms,cs,self)
//                                                               //)
//                                                      );
    }
    
	// other things to do somewhere:
	// - get list of control points
	// - get the control point in the arm base coordinate system
	// - load up a configuration file with ip address to send to, etc.
	boost::asio::io_service& io_service_;
    boost::asio::io_service user_io_service_;
    boost::asio::io_service::strand strand_;
    std::unique_ptr<robot::arm::kuka::iiwa> iiwaP;
    std::unique_ptr<boost::asio::io_service::work> workP;
    std::unique_ptr<std::thread> driver_threadP;
    
    /// @todo have separate sets for full/empty states?
    /// @todo replace with unique_ptr
    
    /// the front is the most recent state, the back is the oldest
    boost::circular_buffer<std::shared_ptr<robot::arm::kuka::iiwa::MonitorState>> monitorStates;
    /// the front is the most recent state, the back is the oldest
    boost::circular_buffer<std::shared_ptr<robot::arm::kuka::iiwa::CommandState>> commandStates;
};

} // namespace robone

#endif
