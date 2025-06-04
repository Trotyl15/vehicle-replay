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
#include <vector>
#include <deque>
#include <filesystem>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imfilebrowser.h"

// ───────────────────────────────────────────────────────────
// window / camera globals
// ───────────────────────────────────────────────────────────
const unsigned SCR_WIDTH = 1200, SCR_HEIGHT = 800;
glm::vec3 cameraPos{0,2.1f,3}, cameraFront{0,-0.05,-1}, cameraUp{0,1,0};
glm::vec3 modelPos{0,2,2.7f};
float modelYaw = 180.0f, deltaTime = 0, lastFrame = 0, fov = 45;
float lastX=400,lastY=300, yaw=-90,pitch=0; bool firstMouse=true;
Assimp::Importer importer;
ImGui::FileBrowser mFileDialog;
std::string mCurrentFile = "< ... >";
// Track visualization
std::deque<glm::vec3> trackPoints;
const size_t MAX_TRACK_POINTS = 1000;  // Maximum number of points to store
GLuint trackVAO, trackVBO;
// UI state
bool showSettings = true;
bool manualMode = true;
bool cameraLocked = true;
char csvFilePath[256] = "";
bool csvFileSelected = false;
// Add after other global variables
GLuint framebuffer, textureColorbuffer;
int viewportWidth = 800, viewportHeight = 800;
bool show3DView = true;
glm::vec3 carColor(32.0f/255.0f, 139.0f/255.0f, 215.0f/255.0f);  // Default blue color

// Lighting parameters
struct Light {
    glm::vec3 direction = glm::vec3(-0.2f, -1.0f, -0.3f);
    glm::vec3 ambient = glm::vec3(0.2f);
    glm::vec3 diffuse = glm::vec3(0.5f);
    glm::vec3 specular = glm::vec3(1.0f);
} light;

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

bool fileExists(const std::string& path) {
    try {
        return std::filesystem::exists(path);
    } catch (const std::exception& e) {
        std::cerr << "Error checking file existence: " << e.what() << std::endl;
        return false;
    }
}

// ───────────────────────────────────────────────────────────
int main()
{
    std::cout << "Starting application..." << std::endl;
    std::cout.flush();

    // Check for required files
    const std::vector<std::string> requiredFiles = {
        "res/shaders/infinite_grid.vs",
        "res/shaders/infinite_grid.fs",
        "res/shaders/model.vs",
        "res/shaders/model.fs",
        "res/shaders/track.vs",
        "res/shaders/track.fs",
        "res/models/car2/scene.gltf",
        "res/models/car3/model.obj"
    };

    std::cout << "Checking required files..." << std::endl;
    std::cout.flush();

    for (const auto& file : requiredFiles) {
        if (!fileExists(file)) {
            std::cerr << "Required file not found: " << file << std::endl;
            return -1;
        }
        std::cout << "Found: " << file << std::endl;
        std::cout.flush();
    }

    std::cout << "All required files found" << std::endl;
    std::cout.flush();

    if(!glfwInit()) { 
        std::cerr << "GLFW init fail" << std::endl;
        std::cerr.flush();
        return -1; 
    }
    std::cout << "GLFW initialized" << std::endl;
    std::cout.flush();

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GL_TRUE);
#endif

    std::cout << "Creating window..." << std::endl;
    std::cout.flush();
    GLFWwindow* win = glfwCreateWindow(SCR_WIDTH,SCR_HEIGHT,"Viewer",nullptr,nullptr);
    if(!win){ 
        std::cerr << "Window creation failed" << std::endl;
        std::cerr.flush();
        return -1; 
    }
    std::cout << "Window created" << std::endl;
    std::cout.flush();

    std::cout << "Making context current..." << std::endl;
    std::cout.flush();
    glfwMakeContextCurrent(win);
    std::cout << "Context made current" << std::endl;
    std::cout.flush();

    std::cout << "Loading GLAD..." << std::endl;
    std::cout.flush();
    if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)){ 
        std::cerr << "GLAD initialization failed" << std::endl;
        std::cerr.flush();
        return -1; 
    }
    std::cout << "GLAD loaded" << std::endl;
    std::cout.flush();

    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;
    std::cout.flush();

    glfwSetFramebufferSizeCallback(win, framebuffer_size_callback);
    glfwSetCursorPosCallback(win, mouse_callback);
    glfwSetScrollCallback(win, scroll_callback);
    std::cout << "Callbacks set" << std::endl;

    // Initialize shaders first
    std::cout << "Loading shaders..." << std::endl;
    Shader* gridShader = nullptr;
    Shader* modelShader = nullptr;
    Shader* trackShader = nullptr;
    try {
        std::cout << "Loading grid shader..." << std::endl;
        gridShader = new Shader("res/shaders/infinite_grid.vs","res/shaders/infinite_grid.fs");
        if (gridShader->ID == 0) {
            std::cerr << "Failed to load grid shader\n";
            return -1;
        }
        std::cout << "Grid shader loaded successfully" << std::endl;

        std::cout << "Loading model shader..." << std::endl;
        modelShader = new Shader("res/shaders/model.vs","res/shaders/model.fs");
        if (modelShader->ID == 0) {
            std::cerr << "Failed to load model shader\n";
            return -1;
        }
        std::cout << "Model shader loaded successfully" << std::endl;

        std::cout << "Loading track shader..." << std::endl;
        trackShader = new Shader("res/shaders/track.vs","res/shaders/track.fs");
        if (trackShader->ID == 0) {
            std::cerr << "Failed to load track shader\n";
            return -1;
        }
        std::cout << "Track shader loaded successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error loading shaders: " << e.what() << std::endl;
        return -1;
    }

    std::cout << "Loading car model..." << std::endl;
    Model* car = nullptr;
    try {
        car = new Model("res/models/car3/model.obj");
        std::cout << "Car model loaded" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error loading car model: " << e.what() << std::endl;
        return -1;
    }

    std::cout << "Initializing OpenGL state..." << std::endl;
    GLuint VAO; 
    glGenVertexArrays(1,&VAO); 
    glBindVertexArray(VAO);
    glEnable(GL_DEPTH_TEST); 
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    std::cout << "OpenGL state initialized" << std::endl;

    // Initialize track visualization
    std::cout << "Initializing track visualization..." << std::endl;
    try {
        glGenVertexArrays(1, &trackVAO);
        glGenBuffers(1, &trackVBO);
        glBindVertexArray(trackVAO);
        glBindBuffer(GL_ARRAY_BUFFER, trackVBO);
        glBufferData(GL_ARRAY_BUFFER, MAX_TRACK_POINTS * sizeof(glm::vec3), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
        glBindVertexArray(0);  // Unbind track VAO
        std::cout << "Track visualization initialized" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error initializing track visualization: " << e.what() << std::endl;
        return -1;
    }

    std::cout << "Starting WebSocket thread..." << std::endl;
    std::thread(runWebSocket).detach();
    std::cout << "WebSocket thread started" << std::endl;

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  // Enable viewports
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    // Load settings from INI file
    ImGui::LoadIniSettingsFromDisk("imgui.ini");
    
    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Create framebuffer for 3D view
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    // Create texture attachment
    glGenTextures(1, &textureColorbuffer);
    glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, viewportWidth, viewportHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureColorbuffer, 0);

    // Create renderbuffer object for depth and stencil attachment
    GLuint rbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, viewportWidth, viewportHeight);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        std::cout << "ERROR::FRAMEBUFFER:: Framebuffer is not complete!" << std::endl;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    std::cout << "Entering main loop..." << std::endl;
    while(!glfwWindowShouldClose(win))
    {
        float now=glfwGetTime(); deltaTime=now-lastFrame; lastFrame=now;
        glfwPollEvents();

        // Start the ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create dock space
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::Begin("DockSpace", nullptr, window_flags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

        // Settings window
        if (showSettings)
        {
            ImGui::Begin("Settings", &showSettings);
            
            // CSV File Selection
            ImGui::Text("CSV File:");
            ImGui::SameLine();
            if (ImGui::Button("Select CSV"))
            {
                mFileDialog.Open();
            }
            ImGui::SameLine();
            ImGui::Text("%s", mCurrentFile.c_str());

            mFileDialog.SetTitle("Open csv");
            mFileDialog.SetTypeFilters({ ".csv" });
            ImGui::Separator();

            // Car Color Selection
            ImGui::Text("Car Color:");
            float color[3] = { carColor.r, carColor.g, carColor.b };
            if (ImGui::ColorEdit3("##CarColor", color))
            {
                carColor = glm::vec3(color[0], color[1], color[2]);
                ImGui::SaveIniSettingsToDisk("imgui.ini");  // Save when color changes
            }
            ImGui::Separator();

            // Lighting Controls
            ImGui::Text("Lighting Controls");
            float lightDir[3] = { light.direction.x, light.direction.y, light.direction.z };
            if (ImGui::SliderFloat3("Light Direction", lightDir, -1.0f, 1.0f))
            {
                light.direction = glm::vec3(lightDir[0], lightDir[1], lightDir[2]);
                ImGui::SaveIniSettingsToDisk("imgui.ini");  // Save when light direction changes
            }
            
            float ambient[3] = { light.ambient.r, light.ambient.g, light.ambient.b };
            if (ImGui::ColorEdit3("Ambient Light", ambient))
            {
                light.ambient = glm::vec3(ambient[0], ambient[1], ambient[2]);
                ImGui::SaveIniSettingsToDisk("imgui.ini");  // Save when ambient light changes
            }
            
            float diffuse[3] = { light.diffuse.r, light.diffuse.g, light.diffuse.b };
            if (ImGui::ColorEdit3("Diffuse Light", diffuse))
            {
                light.diffuse = glm::vec3(diffuse[0], diffuse[1], diffuse[2]);
                ImGui::SaveIniSettingsToDisk("imgui.ini");  // Save when diffuse light changes
            }
            
            float specular[3] = { light.specular.r, light.specular.g, light.specular.b };
            if (ImGui::ColorEdit3("Specular Light", specular))
            {
                light.specular = glm::vec3(specular[0], specular[1], specular[2]);
                ImGui::SaveIniSettingsToDisk("imgui.ini");  // Save when specular light changes
            }
            ImGui::Separator();

            // Mode Selection
            ImGui::Text("Control Mode:");
            if (ImGui::RadioButton("Manual", manualMode))
            {
                manualMode = true;
                ImGui::SaveIniSettingsToDisk("imgui.ini");  // Save when mode changes
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Automatic", !manualMode))
            {
                manualMode = false;
                ImGui::SaveIniSettingsToDisk("imgui.ini");  // Save when mode changes
            }

            ImGui::Separator();

            // Camera Lock Status
            ImGui::Text("Camera: %s", cameraLocked ? "Locked (Press 'L' to unlock)" : "Free (Press 'L' to lock)");

            ImGui::End();
            mFileDialog.Display();
            if (mFileDialog.HasSelected())
            {
                auto file_path = mFileDialog.GetSelected().string();
                mCurrentFile = file_path.substr(file_path.find_last_of("/\\") + 1);
            }
        }

        // 3D View window
        if (show3DView)
        {
            ImGui::Begin("3D View", &show3DView);
            
            // Get the size of the ImGui window
            ImVec2 windowSize = ImGui::GetContentRegionAvail();
            if (windowSize.x != viewportWidth || windowSize.y != viewportHeight)
            {
                viewportWidth = (int)windowSize.x;
                viewportHeight = (int)windowSize.y;
                
                // Resize framebuffer texture
                glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, viewportWidth, viewportHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
                glBindRenderbuffer(GL_RENDERBUFFER, rbo);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, viewportWidth, viewportHeight);
            }

            // Render 3D scene to framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glViewport(0, 0, viewportWidth, viewportHeight);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Calculate view and projection matrices
            glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
            glm::mat4 proj = glm::perspective(glm::radians(fov), (float)viewportWidth/(float)viewportHeight, 0.1f, 100.0f);

            // Render track
            if (!trackPoints.empty()) {
                trackShader->use();
                trackShader->setMat4("projection", proj);
                trackShader->setMat4("view", view);
                trackShader->setMat4("model", glm::mat4(1.0f));
                trackShader->setVec4("trackColor", glm::vec4(11.0f/255.0f, 48.0f/255.0f, 47.0f/255.0f, 1.0f));

                glBindVertexArray(trackVAO);
                glBindBuffer(GL_ARRAY_BUFFER, trackVBO);
                
                std::vector<glm::vec3> points(trackPoints.begin(), trackPoints.end());
                std::vector<glm::vec3> quadVertices;
                const float trackWidth = 0.03f;
                const float overlap = 0.005f;
                const float heightOffset = -0.001f;
                
                for (size_t i = 0; i < points.size() - 1; ++i) {
                    glm::vec3 current = points[i];
                    glm::vec3 next = points[i + 1];
                    current.y += heightOffset;
                    next.y += heightOffset;
                    
                    glm::vec3 dir = glm::normalize(next - current);
                    glm::vec3 perp = glm::normalize(glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)));
                    glm::vec3 offset = perp * trackWidth;
                    glm::vec3 dirOffset = dir * overlap;
                    
                    quadVertices.push_back(current + offset - dirOffset);
                    quadVertices.push_back(current - offset - dirOffset);
                    quadVertices.push_back(next + offset + dirOffset);
                    quadVertices.push_back(current - offset - dirOffset);
                    quadVertices.push_back(next - offset + dirOffset);
                    quadVertices.push_back(next + offset + dirOffset);
                }
                
                glBufferData(GL_ARRAY_BUFFER, quadVertices.size() * sizeof(glm::vec3), quadVertices.data(), GL_DYNAMIC_DRAW);
                glDrawArrays(GL_TRIANGLES, 0, quadVertices.size());
                glBindVertexArray(0);
            }

            // Render grid
            gridShader->use();
            glm::mat4 model = glm::mat4(1.0f);
            glm::mat4 gVP = proj * view * model;
            glUniformMatrix4fv(glGetUniformLocation(gridShader->ID,"gVP"),1,GL_FALSE,glm::value_ptr(gVP));
            glUniform3fv(glGetUniformLocation(gridShader->ID,"gCameraWorldPos"),1,glm::value_ptr(cameraPos));
            glBindVertexArray(VAO);
            glDrawArrays(GL_TRIANGLES,0,6);

            // Render car model
            modelShader->use();
            modelShader->setMat4("projection", proj);
            modelShader->setMat4("view", view);
            modelShader->setVec3("carColor", carColor);
            modelShader->setVec3("viewPos", cameraPos);
            
            // Set lighting uniforms
            modelShader->setVec3("light.direction", light.direction);
            modelShader->setVec3("light.ambient", light.ambient);
            modelShader->setVec3("light.diffuse", light.diffuse);
            modelShader->setVec3("light.specular", light.specular);

            model = glm::mat4(1.0f);
            model = glm::translate(model, modelPos);
            model = glm::scale(model, {0.03f, 0.03f, 0.03f});
            model = glm::rotate(model, glm::radians(modelYaw), {0.0f, 1.0f, 0.0f});
            modelShader->setMat4("model", model);
            car->Draw(*modelShader);

            // Reset framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // Display the rendered texture in ImGui with flipped UV coordinates
            ImGui::Image((ImTextureID)(uintptr_t)textureColorbuffer, 
                        ImVec2(viewportWidth, viewportHeight),
                        ImVec2(0, 1),  // UV0: bottom-left
                        ImVec2(1, 0)); // UV1: top-right

            ImGui::End();
        }

        ImGui::End(); // End the dock space window

        /* update from WS */
        {
            std::lock_guard lk(poseMtx);
            if(pose.valid && !manualMode){
                glm::dvec2 enu=llToENU(pose.lat,pose.lon);
                // Scale the ENU coordinates to match grid (0.025 units = 1 meter)
                modelPos.x = (float)(enu.x * 0.025);
                modelPos.z = (float)(-enu.y * 0.025);  // north→ -Z
                modelYaw   = 180.f - (float)(pose.head*180/M_PI);
                
                glm::vec3 cameraOffset = glm::vec3( 0, 0.1f, 0.3f);
                cameraPos = cameraOffset + modelPos;
                
                std::cout << "E=" << std::fixed << std::setw(8) << std::setprecision(2) << enu.x << " m   N=" << std::setw(8) << enu.y << " m   θ=" << std::setw(8) << pose.head << " rad" << std::endl;
            }
        }
        processInput(win, manualMode);

        // Update track points in both manual and automatic modes
        trackPoints.push_back(modelPos);
        if (trackPoints.size() > MAX_TRACK_POINTS) {
            trackPoints.pop_front();
        }

        int w,h; glfwGetFramebufferSize(win,&w,&h);
        glViewport(0,0,w,h);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        // Render ImGui
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Update and Render additional Platform Windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

        glfwSwapBuffers(win);
    }

    // Cleanup ImGui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::SaveIniSettingsToDisk("imgui.ini");  // Save settings before shutdown
    ImGui::DestroyContext();

    std::cout << "Cleaning up..." << std::endl;
    // Cleanup
    delete gridShader;
    delete modelShader;
    delete trackShader;
    delete car;

    stopWS=true;
    glfwTerminate();
    std::cout << "Application terminated" << std::endl;
    return 0;
}

//──────────────── callbacks ───────────────
void processInput(GLFWwindow* window, bool manual)
{
    if(glfwGetKey(window,GLFW_KEY_ESCAPE)==GLFW_PRESS) glfwSetWindowShouldClose(window,true);
    
    // Toggle camera lock with 'L' key
    static bool lKeyPressed = false;
    if (glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
        if (!lKeyPressed) {
            cameraLocked = !cameraLocked;
            lKeyPressed = true;
        }
    } else {
        lKeyPressed = false;
    }

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
    if(cameraLocked) return;  // Skip mouse input if camera is locked
    
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
