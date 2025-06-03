#define STB_IMAGE_IMPLEMENTATION
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "stb_image.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "shaders/shader.h"
#include "mesh.h"
#include "model.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#include <libwebsockets.h>
#include "car_pose.pb.h"

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>

// ───────────────────────────────────────────────────────────
// window / camera globals
// ───────────────────────────────────────────────────────────
const unsigned SCR_WIDTH = 800, SCR_HEIGHT = 600;
glm::vec3 cameraPos{0,2.1f,3}, cameraFront{0,0,-1}, cameraUp{0,1,0};
glm::vec3 modelPos{0,2,2.7f};
float modelYaw = 180.0f, deltaTime = 0, lastFrame = 0, fov = 45;
float lastX=400,lastY=300, yaw=-90,pitch=0; bool firstMouse=true;
Assimp::Importer importer;

// ───────────────────────────────────────────────────────────
// geo-math constants
// ───────────────────────────────────────────────────────────
constexpr double LAT0 = 37.7955, LON0 = -122.3937;
constexpr double EARTH_R = 6'378'137.0;
constexpr double DEG2RAD = M_PI/180.0;
constexpr double LAT0_RAD = LAT0*DEG2RAD;

inline glm::dvec2 llToENU(double lat, double lon)
{
    double dLat = (lat - LAT0)*DEG2RAD;
    double dLon = (lon - LON0)*DEG2RAD;
    double north = EARTH_R * dLat;
    double east  = EARTH_R * dLon * cos(LAT0_RAD);
    return { east, north };
}

// ───────────────────────────────────────────────────────────
// shared pose from WS thread
// ───────────────────────────────────────────────────────────
struct Pose { double lat,lon,head; bool valid=false; };
Pose pose; std::mutex poseMtx; std::atomic_bool stopWS{false};

// ───────────────────────────────────────────────────────────
// libwebsockets client thread
// ───────────────────────────────────────────────────────────
static int wsCallback(lws* wsi, lws_callback_reasons r, void* user,
                      void* in, size_t len)
{
    if (r == LWS_CALLBACK_CLIENT_RECEIVE)
    {
        vehicle::CarPose msg;
        if (msg.ParseFromArray(in, (int)len))
        {
            std::lock_guard lk(poseMtx);
            pose.lat   = msg.latitude();
            pose.lon   = msg.longitude();
            pose.head  = msg.heading();           // 0 = north, +CW
            pose.valid = true;
        }
        else
        {
            std::cerr << "Failed to parse CarPose message" << std::endl;
        }
    }
    if (r == LWS_CALLBACK_CLIENT_CLOSED ||
        r == LWS_CALLBACK_CLIENT_CONNECTION_ERROR)
    {
        std::cerr << "WebSocket connection closed or error occurred" << std::endl;
        stopWS = true;
    }
    return 0;
}
void runWebSocket()
{
    static const lws_protocols protos[] = {
        { "binary", wsCallback, 0, 1<<16 },
        { nullptr,  nullptr,    0, 0 }
    };
    lws_context_creation_info info{};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protos;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.timeout_secs = 10;
    info.max_http_header_pool = 16;
    info.max_http_header_data = 4096;
    lws_context* ctx = lws_create_context(&info);
    if (!ctx) { std::cerr<<"lws ctx fail\n"; return; }

    lws_client_connect_info ci{};
    ci.context = ctx;
    ci.address = "localhost";
    ci.port    = 8765;
    ci.path    = "/";
    ci.host    = ci.address;
    ci.origin  = ci.address;
    ci.protocol = "binary";
    ci.ssl_connection = LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    ci.pwsi = nullptr;
    
    lws* wsi = lws_client_connect_via_info(&ci);
    if (!wsi) {
        std::cerr << "Failed to create WebSocket connection" << std::endl;
        lws_context_destroy(ctx);
        return;
    }

    std::cout << "WebSocket connection attempt initiated..." << std::endl;

    while (!stopWS) {
        int ret = lws_service(ctx, 50);
        if (ret < 0) {
            std::cerr << "lws_service error: " << ret << std::endl;
            break;
        }
    }
    
    lws_context_destroy(ctx);
    std::cout << "WebSocket connection terminated." << std::endl;
}

// ───────────────────────────────────────────────────────────
// forward decls
// ───────────────────────────────────────────────────────────
void framebuffer_size_callback(GLFWwindow*,int,int);
void mouse_callback(GLFWwindow*,double,double);
void scroll_callback(GLFWwindow*,double,double);
void processInput(GLFWwindow*, bool manual);

// ───────────────────────────────────────────────────────────
int main()
{
    if(!glfwInit()) { std::cerr<<"GLFW init fail\n"; return -1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GL_TRUE);
#endif
    GLFWwindow* win = glfwCreateWindow(SCR_WIDTH,SCR_HEIGHT,"Viewer",nullptr,nullptr);
    if(!win){ std::cerr<<"Window fail\n"; return -1; }
    glfwMakeContextCurrent(win);
    if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)){ std::cerr<<"GLAD fail\n"; return -1; }

    glfwSetFramebufferSizeCallback(win, framebuffer_size_callback);
    glfwSetCursorPosCallback(win, mouse_callback);
    glfwSetScrollCallback(win, scroll_callback);

    Shader gridShader("res/shaders/infinite_grid.vs","res/shaders/infinite_grid.fs");
    Shader modelShader("res/shaders/model.vs","res/shaders/model.fs");
    Model  car("res/models/car2/scene.gltf");

    GLuint VAO; glGenVertexArrays(1,&VAO); glBindVertexArray(VAO);
    glEnable(GL_DEPTH_TEST); glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    std::thread(runWebSocket).detach();

    while(!glfwWindowShouldClose(win))
    {
        float now=glfwGetTime(); deltaTime=now-lastFrame; lastFrame=now;
        glfwPollEvents();

        /* update from WS */
        bool manual=true;
        {
            std::lock_guard lk(poseMtx);
            if(pose.valid){
                glm::dvec2 enu=llToENU(pose.lat,pose.lon);
                // Scale the ENU coordinates to match grid (0.025 units = 1 meter)
                modelPos.x = (float)(enu.x * 0.025);
                modelPos.z = (float)(-enu.y * 0.025);  // north→ -Z
                modelYaw   = 180.f - (float)(pose.head*180/M_PI);
                
                
                glm::vec3 cameraOffset = glm::vec3( 0, 0.1f, 0.3f);
                cameraPos = cameraOffset + modelPos;
                                
                manual=false;
                std::cout << "E=" << std::fixed << std::setw(8) << std::setprecision(2) << enu.x << " m   N=" << std::setw(8) << enu.y << " m   θ=" << std::setw(8) << pose.head << " rad" << std::endl;
            }
        }
        processInput(win, manual);

        int w,h; glfwGetFramebufferSize(win,&w,&h);
        glViewport(0,0,w,h);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        gridShader.use();
        glm::mat4 model = glm::mat4(1.0f);
        glm::mat4 view  = glm::lookAt(cameraPos,cameraPos+cameraFront,cameraUp);
        glm::mat4 proj  = glm::perspective(glm::radians(fov),(float)SCR_WIDTH/(float)SCR_HEIGHT,0.1f,100.0f);
        glm::mat4 gVP   = proj * view * model;

        glUniformMatrix4fv(glGetUniformLocation(gridShader.ID,"gVP"),1,GL_FALSE,glm::value_ptr(gVP));
        glUniform3fv(glGetUniformLocation(gridShader.ID,"gCameraWorldPos"),1,glm::value_ptr(cameraPos));
        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES,0,6);

        // ------------------------------------------------------------------
        //  car model
        // ------------------------------------------------------------------
        modelShader.use();
        modelShader.setMat4("projection",proj);
        modelShader.setMat4("view",view);

        model  = glm::mat4(1.0f);
        model  = glm::translate(model,modelPos);
        model  = glm::scale(model,{0.03f,0.03f,0.03f});
        model  = glm::rotate(model, glm::radians(modelYaw),{0.0f,1.0f,0.0f});
        model  = glm::rotate(model,glm::radians( 90.0f),{-1.0f,0.0f,0.0f});
        modelShader.setMat4("model",model);
        car.Draw(modelShader);

        glfwSwapBuffers(win);
    }
    stopWS=true;
    glfwTerminate();
    return 0;
}

//──────────────── callbacks ───────────────
void processInput(GLFWwindow* window,bool manual)
{
    if(glfwGetKey(window,GLFW_KEY_ESCAPE)==GLFW_PRESS) glfwSetWindowShouldClose(window,true);
    if(!manual) return;

    float speed = 0.2f * deltaTime;  // 0.2 m/s (0.2 * 0.025)
    const float yawRad = glm::radians(modelYaw);     // Y-axis yaw (degrees → rad)
    glm::vec3 carForward = glm::vec3(sin(yawRad), 0.0f, cos(yawRad));
    if (glfwGetKey(window,GLFW_KEY_W)==GLFW_PRESS){ cameraPos += speed*carForward; modelPos += speed*carForward; }
    if (glfwGetKey(window,GLFW_KEY_S)==GLFW_PRESS){ cameraPos -= speed*carForward; modelPos -= speed*carForward; }
    if (glfwGetKey(window,GLFW_KEY_A)==GLFW_PRESS){ modelYaw += 90.0f*deltaTime; }
    if (glfwGetKey(window,GLFW_KEY_D)==GLFW_PRESS){ modelYaw -= 90.0f*deltaTime; }
}
void framebuffer_size_callback(GLFWwindow*,int w,int h){ glViewport(0,0,w,h); }

void mouse_callback(GLFWwindow*,double xpos,double ypos)
{
    if(firstMouse){ lastX=(float)xpos; lastY=(float)ypos; firstMouse=false; }
    float xoff=(float)xpos-lastX, yoff=lastY-(float)ypos; lastX=(float)xpos; lastY=(float)ypos;
    const float sens=0.05f; xoff*=sens; yoff*=sens;
    yaw+=xoff; pitch+=yoff; if(pitch>89.0f) pitch=89.0f; if(pitch<-89.0f) pitch=-89.0f;
    glm::vec3 front;
    front.x = cos(glm::radians(pitch))*cos(glm::radians(yaw));
    front.y = sin(glm::radians(pitch));
    front.z = cos(glm::radians(pitch))*sin(glm::radians(yaw));
    cameraFront = glm::normalize(front);
}

void scroll_callback(GLFWwindow*,double,double yoff)
{
    if(fov>=1.0f && fov<=45.0f) fov -= (float)yoff;
    if(fov<1.0f)  fov = 1.0f;
    if(fov>45.0f) fov = 45.0f;
}
