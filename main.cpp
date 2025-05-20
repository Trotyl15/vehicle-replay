#define STB_IMAGE_IMPLEMENTATION
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "stb_image.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "shaders/shader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#include "mesh.h"
#include "model.h"

#include <iostream>
#include <thread>
#include <chrono>

// ───────────────────────────────────────────────────────────
// forward declarations
// ───────────────────────────────────────────────────────────
void framebuffer_size_callback(GLFWwindow*, int, int);
void processInput(GLFWwindow*);
void mouse_callback(GLFWwindow*, double, double);
void scroll_callback(GLFWwindow*, double, double);

// ───────────────────────────────────────────────────────────
// settings / globals
// ───────────────────────────────────────────────────────────
const unsigned int SCR_WIDTH  = 800;
const unsigned int SCR_HEIGHT = 600;

glm::vec3 cameraPos   = { 0.0f, 2.1f, 3.0f };
glm::vec3 cameraFront = { 0.0f, 0.0f,-1.0f };
glm::vec3 cameraUp    = { 0.0f, 1.0f, 0.0f };

glm::vec3 modelPos = { 0.0f, 2.0f, 2.7f };
float modelYaw   = 180.0f;
float modelSpeed = 2.5f;

float deltaTime = 0.0f;
float lastFrame = 0.0f;

float lastX = 400.0f, lastY = 300.0f;
float yaw   = -90.0f, pitch = 0.0f;
bool  firstMouse = true;

float fov = 45.0f;
Assimp::Importer importer;

// ───────────────────────────────────────────────────────────
// main
// ───────────────────────────────────────────────────────────
int main()
{
    // 1. GLFW init
    if (!glfwInit()) { std::cerr<<"Failed to initialise GLFW\n"; return -1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GL_TRUE);
#endif

    // 2. window
    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH,SCR_HEIGHT,"LearnOpenGL (no-ImGui)",nullptr,nullptr);
    if (!window){ std::cerr<<"Failed to create window\n"; glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);

    // 3. GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)){ std::cerr<<"Failed to init GLAD\n"; return -1; }

    // callbacks
    glfwSetFramebufferSizeCallback(window,framebuffer_size_callback);
    glfwSetCursorPosCallback(window,mouse_callback);
    glfwSetScrollCallback(window,scroll_callback);
    // glfwSetInputMode(window,GLFW_CURSOR,GLFW_CURSOR_DISABLED); // enable if desired

    // 4. shaders / model
    Shader gridShader ("res/shaders/infinite_grid.vs" , "res/shaders/infinite_grid.fs");
    Shader modelShader("res/shaders/model.vs"         , "res/shaders/model.fs");
    Model  carModel  ("res/models/car2/scene.gltf");

    // 5. dummy VAO for the grid
    unsigned int VAO;
    glGenVertexArrays(1,&VAO);
    glBindVertexArray(VAO);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    // 6. main loop
    while(!glfwWindowShouldClose(window))
    {
        // timing
        float current = glfwGetTime();
        deltaTime = current - lastFrame;
        lastFrame = current;

        glfwPollEvents();
        if (glfwGetWindowAttrib(window,GLFW_ICONIFIED))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // clear
        int w,h; glfwGetFramebufferSize(window,&w,&h);
        glViewport(0,0,w,h);
        glClearColor(0.0f,0.0f,0.0f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        // user input
        processInput(window);

        // ------------------------------------------------------------------
        // (A) infinite grid
        // ------------------------------------------------------------------
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
        // (B) car model
        // ------------------------------------------------------------------
        modelShader.use();
        modelShader.setMat4("projection",proj);
        modelShader.setMat4("view",view);

        model  = glm::mat4(1.0f);
        model  = glm::translate(model,modelPos);
        model  = glm::scale(model,{0.03f,0.03f,0.03f});
        model  = glm::rotate(model,glm::radians(180.0f),{0.0f,1.0f,0.0f});
        model  = glm::rotate(model,glm::radians( 90.0f),{-1.0f,0.0f,0.0f});
        modelShader.setMat4("model",model);
        carModel.Draw(modelShader);

        glfwSwapBuffers(window);
    }

    // cleanup
    glDeleteVertexArrays(1,&VAO);
    glfwTerminate();
    return 0;
}

// ───────────────────────────────────────────────────────────
// input / callbacks
// ───────────────────────────────────────────────────────────
void processInput(GLFWwindow* window)
{
    if (glfwGetKey(window,GLFW_KEY_ESCAPE)==GLFW_PRESS)
        glfwSetWindowShouldClose(window,true);

    float speed = 1.0f * deltaTime;
    glm::vec3 horizFront = glm::normalize(glm::vec3(cameraFront.x,0.0f,cameraFront.z));
    glm::vec3 horizRight = glm::normalize(glm::cross(horizFront,cameraUp));

    if (glfwGetKey(window,GLFW_KEY_W)==GLFW_PRESS){ cameraPos += speed*horizFront; modelPos += speed*horizFront; }
    if (glfwGetKey(window,GLFW_KEY_S)==GLFW_PRESS){ cameraPos -= speed*horizFront; modelPos -= speed*horizFront; }
    if (glfwGetKey(window,GLFW_KEY_A)==GLFW_PRESS){ cameraPos -= speed*horizRight; modelPos -= speed*horizRight; modelYaw += 90.0f*deltaTime; }
    if (glfwGetKey(window,GLFW_KEY_D)==GLFW_PRESS){ cameraPos += speed*horizRight; modelPos += speed*horizRight; modelYaw -= 90.0f*deltaTime; }
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
