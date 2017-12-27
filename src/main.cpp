#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "MPC.h"
#include "json.hpp"

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.rfind("}]");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

// Evaluate a polynomial.
double polyeval(Eigen::VectorXd coeffs, double x) {
  double result = 0.0;
  for (int i = 0; i < coeffs.size(); i++) {
    result += coeffs[i] * pow(x, i);
  }
  return result;
}

// Fit a polynomial.
// Adapted from
// https://github.com/JuliaMath/Polynomials.jl/blob/master/src/Polynomials.jl#L676-L716
Eigen::VectorXd polyfit(Eigen::VectorXd xvals, Eigen::VectorXd yvals,
                        int order) {
  assert(xvals.size() == yvals.size());
  assert(order >= 1 && order <= xvals.size() - 1);
  Eigen::MatrixXd A(xvals.size(), order + 1);

  for (int i = 0; i < xvals.size(); i++) {
    A(i, 0) = 1.0;
  }

  for (int j = 0; j < xvals.size(); j++) {
    for (int i = 0; i < order; i++) {
      A(j, i + 1) = A(j, i) * xvals(j);
    }
  }

  auto Q = A.householderQr();
  auto result = Q.solve(yvals);
  return result;
}

vector<double> global2car(double psi, double px, double py, double x_global, double y_global)
{
    double dx = x_global - px;
    double dy = y_global - py;
    double sin_psi = sin(psi);
    double cos_psi = cos(psi);

    double x_car = dx*cos_psi + dy*sin_psi;
    double y_car = - dx*sin_psi + dy*cos_psi;

    return {x_car, y_car};
}


int main() {
  uWS::Hub h;

  // MPC is initialized here!
  MPC mpc;

  h.onMessage([&mpc](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    string sdata = string(data).substr(0, length);
    cout << sdata << endl;
    if (sdata.size() > 2 && sdata[0] == '4' && sdata[1] == '2') {
      string s = hasData(sdata);
      if (s != "") {
        auto j = json::parse(s);
        string event = j[0].get<string>();
        if (event == "telemetry") {
          // j[1] is the data JSON object
          vector<double> ptsx = j[1]["ptsx"];
          vector<double> ptsy = j[1]["ptsy"];
          double px = j[1]["x"];
          double py = j[1]["y"];
          double psi = j[1]["psi"];
          double v = j[1]["speed"];
          double delta = j[1]["steering_angle"];
          double acceleration = j[1]["throttle"];

          v = v * 0.44704; // convert to m/s from mph
          delta = -delta; // convert steering angle delta sign from simulator

          // predict state in 100ms using kinematic model
          double latency = 0.1;
          double Lf = 2.67;
          px = px + v*cos(psi)*latency;     // x_[t+1] = x[t] + v[t] * cos(psi[t]) * dt
          py = py + v*sin(psi)*latency;     // y_[t+1] = y[t] + v[t] * sin(psi[t]) * dt
          psi = psi + v*delta/Lf*latency;   // psi_[t+1] = psi[t] + v[t] / Lf * delta[t] * dt
          v = v + acceleration*latency;     // v_[t+1] = v[t] + a[t] * dt

          Eigen::VectorXd way_pts_x(ptsx.size());
          Eigen::VectorXd way_pts_y(ptsx.size());

          // Tranform waypoints to car co-ordinates. 
          // Remainder of the calculations are done in car co-ordinate system
          for(size_t i=0; i < ptsx.size(); i++) {
            auto coord_car = global2car(psi, px, py, ptsx[i], ptsy[i]);
            way_pts_x(i) = coord_car[0];
            way_pts_y(i) = coord_car[1];
          }

          // Fit a third order polynomial to way points to
          // model the reference trajectory
          auto coeffs = polyfit(way_pts_x, way_pts_y, 3);

          // Since we are in the car co-ordinate system the cross track error
          // is simply the y co-ordinate of the reference trajectory at x = 0
          double cte = polyeval(coeffs,0);
          // For the same reason error in yaw angle is the direction of the
          // reference trajectory at x = 0. i.e. arctangent of the derivative
          // of the reference trajectory
          double epsi = -atan(coeffs[1]);


          /*
          State variables 
             x and y positions of car
             yaw angle
             speed in heading direction
             cross track error
             yaw angle error

          Since we are in car-cordinate system the cars position (x,y) and yaw angle (psi) are all 0.
          Since car-cordinate system has the same scale as the global system v does not change
          */
          Eigen::VectorXd state(6);
          state << 0, 0, 0, v, cte, epsi;

          /*
          * Calculate steering angle and throttle using MPC.
          * Both are in between [-1, 1].
          *
          */
          auto result = mpc.Solve(state, coeffs);

          // Apply the first actuation values from the solver to the car
          double steer_value = -result[0];
          double throttle_value = result[1];

          json msgJson;
          // NOTE: Remember to divide by deg2rad(25) before you send the steering value back.
          // Otherwise the values will be in between [-deg2rad(25), deg2rad(25] instead of [-1, 1].
          msgJson["steering_angle"] = steer_value/deg2rad(25);
          msgJson["throttle"] = throttle_value;

          //Display the MPC predicted trajectory 
          vector<double> mpc_x_vals;
          vector<double> mpc_y_vals;

          //.. add (x,y) points to list here, points are in reference to the vehicle's coordinate system
          // the points in the simulator are connected by a Green line
          size_t n = (result.size()-2)/2;
          for (size_t i = 0; i < n; i ++) {
              mpc_x_vals.push_back(result[i + 2]);
              mpc_y_vals.push_back(result[i + n + 2]);
          }

          msgJson["mpc_x"] = mpc_x_vals;
          msgJson["mpc_y"] = mpc_y_vals;

          //Display the waypoints/reference line
          vector<double> next_x_vals;
          vector<double> next_y_vals;

          //.. add (x,y) points to list here, points are in reference to the vehicle's coordinate system
          // the points in the simulator are connected by a Yellow line
          for (int i = 0; i < way_pts_x.size(); i += 1){
            //next_x_vals.push_back(way_pts_x(i));
            //next_y_vals.push_back(way_pts_y(i));
          }

          for (double i = 0; i < 100.0; i += 2){
            next_x_vals.push_back(i);
            next_y_vals.push_back(polyeval(coeffs, i));
          }

          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;


          auto msg = "42[\"steer\"," + msgJson.dump() + "]";
          std::cout << msg << std::endl;
          // Latency
          // The purpose is to mimic real driving conditions where
          // the car does actuate the commands instantly.
          //
          // Feel free to play around with this value but should be to drive
          // around the track with 100ms latency.
          //
          // NOTE: REMEMBER TO SET THIS TO 100 MILLISECONDS BEFORE
          // SUBMITTING.
          this_thread::sleep_for(chrono::milliseconds(static_cast<int>(latency*1000)));
          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
