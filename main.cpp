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
#include <fstream>
#include <sstream>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imfilebrowser.h"

#include "map_tiles.h"

// ───────────────────────────────────────────────────────────
// window / camera globals
// ───────────────────────────────────────────────────────────
const unsigned SCR_WIDTH = 1200, SCR_HEIGHT = 800;
glm::vec3 cameraPos{0,2.1f,3}, cameraFront{0,-0.05,-1}, cameraUp{0,1,0};
glm::vec3 modelPos{0,2,2.7f};
glm::vec3 targetModelPos{0,2,2.7f};   // interpolation target
float modelYaw = 180.0f, targetModelYaw = 180.0f;
float deltaTime = 0, lastFrame = 0, fov = 45;
const float LERP_SPEED = 10.0f;  // interpolation speed
float lastX=400,lastY=300, yaw=-90,pitch=0; bool firstMouse=true;
Assimp::Importer importer;
ImGui::FileBrowser mFileDialog;
std::string mCurrentFile = "< ... >";
std::string mSelectedCsvPath;

// Track visualization
std::deque<glm::vec3> trackPoints;
const size_t MAX_TRACK_POINTS = 10000;  // Maximum number of points to store
GLuint trackVAO, trackVBO;
// UI state
bool showSettings = true;
bool manualMode = true;
bool replayMode = false;  // New mode for CSV replay
bool cameraLocked = true;
char csvFilePath[256] = "";
bool csvFileSelected = false;
float replayProgress = 0.0f;  // Progress bar value (0.0 to 1.0)
// Add after other global variables
GLuint framebuffer, textureColorbuffer;
int viewportWidth = 800, viewportHeight = 800;
bool show3DView = true;
glm::vec3 carColor(32.0f/255.0f, 139.0f/255.0f, 215.0f/255.0f);  // Default blue color
bool topDownView = false;  // New camera view mode
glm::vec3 topDownOffset(0.0f, 5.0f, 0.0f);  // Camera offset for top-down view
glm::vec3 followOffset(0.0f, 0.1f, 0.3f);   // Camera offset for follow view
glm::vec3 lockedCameraFront{0.0f, 0.0f, -1.0f};  // Store the camera orientation when locked
bool wasLocked = false;  // Track if camera was previously locked
bool isDragging = false;  // Track if we're currently dragging
glm::vec2 lastDragPos;   // Last mouse position during drag

// Map tile manager
MapTileManager mapTiles;
GLuint mapTileVAO = 0, mapTileVBO = 0;
Shader* mapTileShader = nullptr;
double currentLat = 0.0, currentLon = 0.0;  // current GPS for UI display

// Traffic cone placement
Model* coneModel = nullptr;
std::vector<glm::vec3> conePlacements;  // world-space positions of placed cones
bool showConeContextMenu = false;
glm::vec3 pendingConePos(0.0f);         // world pos under the right-click
ImVec2 viewportOrigin(0,0);             // screen pos of the 3D viewport origin

// Function to update camera position and orientation
void updateCamera() {
    if (topDownView) {
        if (cameraLocked) {
            // In top-down view, camera follows car from above only when locked
            cameraPos = modelPos + topDownOffset;
            cameraFront = glm::vec3(0.0f, -1.0f, 0.0f);  // Look straight down
            cameraUp = glm::vec3(0.0f, 0.0f, -1.0f);     // Adjust up vector for top-down view
        }
        // When unlocked in top-down view, keep camera position and orientation unchanged
    } else {
        // Calculate camera position based on car's position and orientation
        const float yawRad = glm::radians(modelYaw);
        glm::vec3 carForward = glm::vec3(sin(yawRad), 0.0f, cos(yawRad));
        glm::vec3 carRight = glm::vec3(cos(yawRad), 0.0f, -sin(yawRad));
        
        // Position camera behind and slightly above the car
        cameraPos = modelPos + followOffset;
        
        if (cameraLocked) {
            // Store the current camera orientation when first entering locked mode
            if (!wasLocked) {
                lockedCameraFront = cameraFront;
                wasLocked = true;
            }
            // Maintain the stored camera orientation
            cameraFront = lockedCameraFront;
            cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
        } else {
            wasLocked = false;
            // In unlocked mode, keep the current cameraFront (set by mouse movement)
            // but ensure camera follows the car's position
            cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }
}

// Lighting parameters
struct Light {
    glm::vec3 direction = glm::vec3(-0.2f, -1.0f, -0.3f);
    glm::vec3 ambient = glm::vec3(0.2f);
    glm::vec3 diffuse = glm::vec3(0.5f);
    glm::vec3 specular = glm::vec3(1.0f);
} light;

// Function to save lighting settings
void SaveLightingSettings() {
    std::ofstream outFile("lighting.ini");
    if (outFile.is_open()) {
        outFile << "Direction=" << light.direction.x << "," << light.direction.y << "," << light.direction.z << "\n";
        outFile << "Ambient=" << light.ambient.x << "," << light.ambient.y << "," << light.ambient.z << "\n";
        outFile << "Diffuse=" << light.diffuse.x << "," << light.diffuse.y << "," << light.diffuse.z << "\n";
        outFile << "Specular=" << light.specular.x << "," << light.specular.y << "," << light.specular.z << "\n";
        outFile.close();
    }
}

// Function to load lighting settings
void LoadLightingSettings() {
    std::ifstream inFile("lighting.ini");
    if (!inFile.is_open()) return;

    std::string line;
    while (std::getline(inFile, line)) {
        std::istringstream iss(line);
        std::string key, value;
        if (std::getline(iss, key, '=') && std::getline(iss, value)) {
            std::istringstream valueStream(value);
            std::string x, y, z;
            
            if (std::getline(valueStream, x, ',') && 
                std::getline(valueStream, y, ',') && 
                std::getline(valueStream, z)) {
                
                if (key == "Direction") {
                    light.direction = glm::vec3(std::stof(x), std::stof(y), std::stof(z));
                } else if (key == "Ambient") {
                    light.ambient = glm::vec3(std::stof(x), std::stof(y), std::stof(z));
                } else if (key == "Diffuse") {
                    light.diffuse = glm::vec3(std::stof(x), std::stof(y), std::stof(z));
                } else if (key == "Specular") {
                    light.specular = glm::vec3(std::stof(x), std::stof(y), std::stof(z));
                }
            }
        }
    }
    inFile.close();
}

// ───────────────────────────────────────────────────────────
// geo-math constants
// ───────────────────────────────────────────────────────────
double LAT0 = 37.7955, LON0 = -122.3937;
bool   originSet = false;          // true once LAT0/LON0 have been set from data
constexpr double EARTH_R = 6'378'137.0;
constexpr double DEG2RAD = M_PI/180.0;

inline glm::dvec2 llToENU(double lat, double lon)
{
    // Auto-set origin to the first valid coordinate we receive
    if (!originSet && lat != 0.0 && lon != 0.0) {
        LAT0 = lat;
        LON0 = lon;
        originSet = true;
        mapTiles.setReference(lat, lon);
    }
    double lat0Rad = LAT0 * DEG2RAD;
    double dLat = (lat - LAT0)*DEG2RAD;
    double dLon = (lon - LON0)*DEG2RAD;
    double north = EARTH_R * dLat;
    double east  = EARTH_R * dLon * cos(lat0Rad);
    return { east, north };
}

// ───────────────────────────────────────────────────────────
// shared pose from WS & CSV threads
// ───────────────────────────────────────────────────────────
struct Pose { double lat,lon,head; bool valid=false; };
Pose pose; std::mutex poseMtx; std::atomic_bool stopWS{false};

// ───────────────────────────────────────────────────────────
// CSV playback helper
// ───────────────────────────────────────────────────────────
class CSVPlayer {
public:
    CSVPlayer() : stopFlag(false), isPaused(false), seeking(false), totalRows(0), currentRow(0), totalTime(0.0), currentTime(0.0) {}
    ~CSVPlayer(){ stopAndJoin(); }

    // Add new method to get current row data
    std::vector<std::string> getCurrentRowData() const {
        if (currentRow < rows.size()) {
            return rows[currentRow];
        }
        return std::vector<std::string>();
    }

    // Add method to get header names
    std::vector<std::string> getHeaders() const {
        return headers;
    }

    void play(const std::string& path, bool loop=true) {
        stopAndJoin();
        stopFlag = false;
        isPaused = false;
        seeking = false;
        totalRows = 0;
        currentRow = 0;
        totalTime = 0.0;
        currentTime = 0.0;
        th = std::thread([this, path, loop]{ this->worker(path, loop); });
    }
    
    void stopAndJoin(){
        stopFlag = true;
        if(th.joinable()) th.join();
    }
    
    void togglePause() {
        isPaused = !isPaused;
    }
    
    bool isPlaying() const {
        return !isPaused && !stopFlag;
    }

    std::atomic<float> playbackSpeed{1.0f};  // default 1x speed
    std::atomic_bool snapRequested{false};
    
    float getProgress() const {
        if (totalTime <= 0.0) return 0.0f;
        return static_cast<float>(currentTime / totalTime);
    }

    std::string getTimeString() const {
        int currentMinutes = static_cast<int>(currentTime) / 60;
        int currentSeconds = static_cast<int>(currentTime) % 60;
        int totalMinutes = static_cast<int>(totalTime) / 60;
        int totalSeconds = static_cast<int>(totalTime) % 60;
        
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%02d:%02d / %02d:%02d", 
                currentMinutes, currentSeconds, totalMinutes, totalSeconds);
        return std::string(buffer);
    }
    
    void seekTo(float progress) {
        if (totalTime <= 0.0) return;
        double targetTime = progress * totalTime;
        if (targetTime <= totalTime) {
            seeking = true;
            currentTime = targetTime;
            // Find the closest row to this time
            if (rows.size() > 0) {
                size_t targetRow = 0;
                double firstTime = std::stod(rows[0][timeIndex]);
                for (size_t i = 0; i < rows.size(); ++i) {
                    double rowTime = std::stod(rows[i][timeIndex]) - firstTime;
                    if (timeInUs) rowTime /= 1e6;
                    if (rowTime >= targetTime) {
                        targetRow = i;
                        break;
                    }
                }
                currentRow = targetRow;
            }
        }
    }
    
private:
    void worker(const std::string& path, bool loop){
        using namespace std::chrono;
        while(!stopFlag){
            std::ifstream ifs(path);
            if(!ifs.is_open()){
                std::cerr << "CSVPlayer: cannot open " << path << "\n";
                return;
            }
            std::string line;
            // Skip comment lines
            while(std::getline(ifs,line)){
                if(!line.empty() && line[0] != '#'){
                    break; // first header line reached
                }
            }
            if(ifs.eof()) return;
            // Build header vector and find indices
            headers.clear(); headers.reserve(32);  // Make headers accessible
            {
                std::stringstream ss(line);
                std::string cell;
                while(std::getline(ss,cell,',')){
                    cell.erase(std::remove(cell.begin(),cell.end(),'\"'),cell.end());
                    headers.push_back(cell);
                }
            }
            auto idxOf=[&](const std::string& key)->int{
                for(size_t i=0;i<headers.size();++i) if(headers[i]==key) return (int)i;
                return -1;
            };
            timeIndex = idxOf("Time");
            if (timeIndex < 0) timeIndex = idxOf("timestamp_us");
            bool timeInMicroseconds = (timeIndex >= 0 && headers[timeIndex] == "timestamp_us");
            timeInUs = timeInMicroseconds;

            int idxLat  = idxOf("Latitude");
            if (idxLat < 0) idxLat = idxOf("lat_synth");
            int idxLon  = idxOf("Longitude");
            if (idxLon < 0) idxLon = idxOf("lon_synth");
            int idxHead = idxOf("Heading");
            if (idxHead < 0) idxHead = idxOf("ground_track_deg");
            if (idxHead < 0) idxHead = idxOf("vehicle_heading_deg");

            if(timeIndex<0||idxLat<0||idxLon<0||idxHead<0){
                std::cerr << "CSVPlayer: header missing required fields (need Time/timestamp_us, Latitude/lat_synth, Longitude/lon_synth, Heading/vehicle_heading_deg)\n";
                return;
            }
            // Reset the origin so it recalculates from this dataset
            originSet = false;
            lastGpsLat = 0; lastGpsLon = 0; hasLastGps = false;
            rows.clear();
            // read rest of file
            while(std::getline(ifs,line)){
                if(line.empty()||line[0]=='#') continue;
                std::vector<std::string> row; row.reserve(headers.size());
                std::stringstream ss(line);
                std::string cell;
                while(std::getline(ss,cell,',')) row.push_back(cell);
                if(row.size()==headers.size()) rows.push_back(std::move(row));
            }
            
            totalRows = rows.size();
            currentRow = 0;
            
            // Calculate total time
            if (!rows.empty()) {
                double firstTime = std::stod(rows[0][timeIndex]);
                double lastTime = std::stod(rows.back()[timeIndex]);
                double rawTotal = lastTime - firstTime;
                totalTime = timeInMicroseconds ? rawTotal / 1e6 : rawTotal;
                currentTime = 0.0;
            }
            
            // Playback rows
            double prevSimT = 0.0;
            bool first = true;
            for(size_t i = currentRow; i < rows.size(); ++i) {
                if(stopFlag) return;
                
                // Handle pausing
                while(isPaused && !stopFlag) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if(stopFlag) return;
                
                // Handle seeking
                if (seeking) {
                    i = currentRow;  // Jump to the new position
                    first = true;    // Reset first flag to avoid sleep
                    prevSimT = std::stod(rows[i][timeIndex]);  // Update prevSimT
                    // Seed previous GPS from the row before the seek target
                    // so heading is computed correctly from the start
                    hasLastGps = false;
                    for (size_t back = i; back > 0; --back) {
                        double bLat = std::stod(rows[back-1][idxLat]);
                        double bLon = std::stod(rows[back-1][idxLon]);
                        if (bLat != 0.0 || bLon != 0.0) {
                            lastGpsLat = bLat; lastGpsLon = bLon;
                            hasLastGps = true;
                            break;
                        }
                    }
                    snapRequested = true;
                    seeking = false;  // Reset seeking state
                }
                
                auto& r = rows[i];
                currentRow = i;
                double simT = std::stod(r[timeIndex]);
                double rawElapsed = simT - std::stod(rows[0][timeIndex]);
                currentTime = timeInMicroseconds ? rawElapsed / 1e6 : rawElapsed;
                
                if(!first){
                    double dt = simT - prevSimT;
                    if (timeInMicroseconds) dt /= 1e6;
                    float spd = playbackSpeed.load();
                    if (spd > 0.0f) dt /= spd;
                    if(dt>0) std::this_thread::sleep_for(duration<double>(dt));
                }
                prevSimT = simT; first=false;

                double lat = std::stod(r[idxLat]);
                double lon = std::stod(r[idxLon]);
                // Skip rows with invalid (zero) GPS coordinates
                if (lat == 0.0 && lon == 0.0) continue;

                Pose p; p.lat = lat; p.lon = lon;

                // Compute heading from consecutive GPS positions
                // This is more reliable than the heading column which may be an IMU heading
                if (hasLastGps) {
                    double dLat = lat - lastGpsLat;
                    double dLon = lon - lastGpsLon;
                    double dist = sqrt(dLat*dLat + dLon*dLon);
                    if (dist > 1e-8) {  // only update heading if there's meaningful movement
                        // atan2(dEast, dNorth) gives compass bearing in radians
                        double bearing = atan2(dLon * cos(lat * DEG2RAD), dLat);
                        p.head = bearing;  // already in radians, 0=North, +CW
                    } else {
                        // Not enough movement, use column value as fallback
                        double headingVal = std::stod(r[idxHead]);
                        p.head = headingVal * DEG2RAD;
                    }
                } else {
                    double headingVal = std::stod(r[idxHead]);
                    p.head = headingVal * DEG2RAD;
                }
                lastGpsLat = lat; lastGpsLon = lon; hasLastGps = true;

                p.valid = true;
                {
                    std::lock_guard lk(poseMtx);
                    pose = p; // overwrite global pose
                }
            }
            if(!loop) break;
            currentRow = 0;  // Reset for next loop
            currentTime = 0.0;
        }
    }
    std::thread th; 
    std::atomic_bool stopFlag{false};
    std::atomic_bool isPaused{false};
    std::atomic_bool seeking{false};
    std::atomic<size_t> totalRows{0};
    std::atomic<size_t> currentRow{0};
    std::atomic<double> totalTime{0.0};
    std::atomic<double> currentTime{0.0};
    std::vector<std::vector<std::string>> rows;
    int timeIndex = -1;
    bool timeInUs = false;             // true when column is timestamp_us
    double lastGpsLat = 0, lastGpsLon = 0;
    bool hasLastGps = false;
    std::vector<std::string> headers;  // Add headers as member variable
};
CSVPlayer csvPlayer;

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
            pose.head  = msg.heading();           // 0 = north, +CW (rad)
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
        "res/shaders/map_tile.vs",
        "res/shaders/map_tile.fs",
        "res/models/car2/scene.gltf",
        "res/models/car3/model.obj",
        "res/cone/1.obj"
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

        std::cout << "Loading map tile shader..." << std::endl;
        mapTileShader = new Shader("res/shaders/map_tile.vs","res/shaders/map_tile.fs");
        if (mapTileShader->ID == 0) {
            std::cerr << "Failed to load map tile shader\n";
            return -1;
        }
        std::cout << "Map tile shader loaded successfully" << std::endl;
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

    std::cout << "Loading cone model..." << std::endl;
    try {
        coneModel = new Model("res/cone/1.obj");
        std::cout << "Cone model loaded" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error loading cone model: " << e.what() << std::endl;
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

    // Initialize map tile quad (two triangles with positions + UVs)
    {
        glGenVertexArrays(1, &mapTileVAO);
        glGenBuffers(1, &mapTileVBO);
        glBindVertexArray(mapTileVAO);
        glBindBuffer(GL_ARRAY_BUFFER, mapTileVBO);
        // 6 vertices × 5 floats (x,y,z, u,v) – will be filled per-tile
        glBufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
        glBindVertexArray(0);
        std::cout << "Map tile quad initialized" << std::endl;
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
    LoadLightingSettings();  // Load our custom lighting settings
    
    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Create framebuffer for 3D view
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    // Create texture attachment
    glGenTextures(1, &textureColorbuffer);
    glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, viewportWidth, viewportWidth, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureColorbuffer, 0);

    // Create renderbuffer object for depth and stencil attachment
    GLuint rbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, viewportWidth, viewportWidth);
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

            // Camera View Selection
            ImGui::Text("Camera View:");
            if (ImGui::RadioButton("Follow", !topDownView))
            {
                topDownView = false;
                ImGui::SaveIniSettingsToDisk("imgui.ini");
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Top-Down", topDownView))
            {
                topDownView = true;
                ImGui::SaveIniSettingsToDisk("imgui.ini");
            }
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
                SaveLightingSettings();
            }
            
            float ambient[3] = { light.ambient.r, light.ambient.g, light.ambient.b };
            if (ImGui::ColorEdit3("Ambient Light", ambient))
            {
                light.ambient = glm::vec3(ambient[0], ambient[1], ambient[2]);
                SaveLightingSettings();
            }
            
            float diffuse[3] = { light.diffuse.r, light.diffuse.g, light.diffuse.b };
            if (ImGui::ColorEdit3("Diffuse Light", diffuse))
            {
                light.diffuse = glm::vec3(diffuse[0], diffuse[1], diffuse[2]);
                SaveLightingSettings();
            }
            
            float specular[3] = { light.specular.r, light.specular.g, light.specular.b };
            if (ImGui::ColorEdit3("Specular Light", specular))
            {
                light.specular = glm::vec3(specular[0], specular[1], specular[2]);
                SaveLightingSettings();
            }
            ImGui::Separator();

            // Mode Selection
            ImGui::Text("Control Mode:");
            if (ImGui::RadioButton("Manual", manualMode))
            {
                manualMode = true;
                replayMode = false;
                csvPlayer.stopAndJoin();  // Stop any ongoing replay
                ImGui::SaveIniSettingsToDisk("imgui.ini");
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Live", !manualMode && !replayMode))
            {
                manualMode = false;
                replayMode = false;
                csvPlayer.stopAndJoin();  // Stop any ongoing replay
                ImGui::SaveIniSettingsToDisk("imgui.ini");
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Replay", replayMode))
            {
                manualMode = false;
                replayMode = true;
                ImGui::SaveIniSettingsToDisk("imgui.ini");
            }

            ImGui::Separator();

            // Camera Lock Status
            ImGui::Text("Camera: %s", cameraLocked ? "Locked (Press 'L' to unlock)" : "Free (Press 'L' to lock)");

            ImGui::Separator();

            // Map Controls
            ImGui::Text("Map Settings:");
            ImGui::Checkbox("Show Map", &mapTiles.enabled);
            if (mapTiles.enabled) {
                ImGui::SliderInt("Zoom Level", &mapTiles.zoomLevel, 15, 20);
            }

            ImGui::Separator();

            // Lat/Lon display
            ImGui::Text("GPS Position:");
            ImGui::Text("Lat:  %.8f", currentLat);
            ImGui::Text("Lon: %.8f", currentLon);

            ImGui::End();
            mFileDialog.Display();
            if (mFileDialog.HasSelected())
            {
                auto file_path = mFileDialog.GetSelected().string();
                mCurrentFile = file_path.substr(file_path.find_last_of("/\\") + 1);
                mSelectedCsvPath = file_path;
                csvFileSelected = true;
                // Only start CSV playback if in replay mode
                if (replayMode) {
                    csvPlayer.play(file_path, true); // loop playback
                }
                mFileDialog.ClearSelected();
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

            // Render map tiles (before grid so grid can overlay)
            if (mapTiles.enabled && mapTileShader) {
                mapTiles.clearRenderList();
                glm::mat4 vp = proj * view;
                mapTiles.render(vp, mapTileVAO);

                mapTileShader->use();
                mapTileShader->setInt("uTileTex", 0);
                glActiveTexture(GL_TEXTURE0);
                glBindVertexArray(mapTileVAO);

                const float tileY = 2.0f;  // same Y as car base and grid plane

                for (auto& tile : mapTiles.tileRenderList) {
                    // Build a quad: NW corner = (x0, tileY, z0), SE corner = (x1, tileY, z1)
                    float verts[] = {
                        // pos(x,y,z), uv(u,v)
                        tile.x0, tileY, tile.z0,  0.0f, 0.0f,  // NW  (top-left in UV)
                        tile.x1, tileY, tile.z0,  1.0f, 0.0f,  // NE
                        tile.x1, tileY, tile.z1,  1.0f, 1.0f,  // SE
                        tile.x0, tileY, tile.z0,  0.0f, 0.0f,  // NW
                        tile.x1, tileY, tile.z1,  1.0f, 1.0f,  // SE
                        tile.x0, tileY, tile.z1,  0.0f, 1.0f,  // SW
                    };

                    mapTileShader->setMat4("uVP", vp);
                    glBindTexture(GL_TEXTURE_2D, tile.tex);
                    glBindBuffer(GL_ARRAY_BUFFER, mapTileVBO);
                    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                }
                glBindVertexArray(0);
            }

            // Render grid (only when map tiles are disabled)
            glm::mat4 model = glm::mat4(1.0f);
            if (!mapTiles.enabled) {
                gridShader->use();
                glm::mat4 gVP = proj * view * model;
                glUniformMatrix4fv(glGetUniformLocation(gridShader->ID,"gVP"),1,GL_FALSE,glm::value_ptr(gVP));
                glUniform3fv(glGetUniformLocation(gridShader->ID,"gCameraWorldPos"),1,glm::value_ptr(cameraPos));
                glBindVertexArray(VAO);
                glDrawArrays(GL_TRIANGLES,0,6);
            }

            // Render track lines (on top of map tiles)
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
                const float heightOffset = 0.001f;  // slightly above tiles
                
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

            // Render placed traffic cones
            if (coneModel) {
                // Reuse model shader with an orange color for cones
                glm::vec3 coneColor(1.0f, 0.35f, 0.0f);  // safety orange
                modelShader->setVec3("carColor", coneColor);
                for (auto& cpos : conePlacements) {
                    model = glm::mat4(1.0f);
                    model = glm::translate(model, cpos);
                    model = glm::scale(model, glm::vec3(0.003f));  // OBJ is in cm, scale down
                    modelShader->setMat4("model", model);
                    coneModel->Draw(*modelShader);
                }
            }

            // Reset framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // Display the rendered texture in ImGui with flipped UV coordinates
            // Record viewport position for mouse mapping
            viewportOrigin = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(uintptr_t)textureColorbuffer, 
                        ImVec2(viewportWidth, viewportHeight),
                        ImVec2(0, 1),  // UV0: bottom-left
                        ImVec2(1, 0)); // UV1: top-right

            // ── Right-click to place cone ──
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                // Get mouse pos relative to the viewport image
                ImVec2 mouseScreen = ImGui::GetMousePos();
                float mx = mouseScreen.x - viewportOrigin.x;
                float my = mouseScreen.y - viewportOrigin.y;

                // Normalised device coords (-1 to 1)
                float ndcX =  (2.0f * mx / viewportWidth)  - 1.0f;
                float ndcY = -(2.0f * my / viewportHeight) + 1.0f; // flip Y

                // Unproject ray from camera through click point
                glm::mat4 invVP = glm::inverse(proj * view);
                glm::vec4 nearPt = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
                glm::vec4 farPt  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
                nearPt /= nearPt.w;
                farPt  /= farPt.w;

                glm::vec3 rayOrigin(nearPt);
                glm::vec3 rayDir = glm::normalize(glm::vec3(farPt) - glm::vec3(nearPt));

                // Intersect with ground plane  Y = 2.0  (the grid / tile plane)
                const float groundY = 2.0f;
                if (std::abs(rayDir.y) > 1e-6f) {
                    float t = (groundY - rayOrigin.y) / rayDir.y;
                    if (t > 0.0f) {
                        pendingConePos = rayOrigin + t * rayDir;
                        pendingConePos.y = groundY;
                        showConeContextMenu = true;
                        ImGui::OpenPopup("ConeContextMenu");
                    }
                }
            }

            // Context menu popup
            if (ImGui::BeginPopup("ConeContextMenu")) {
                if (ImGui::MenuItem("Place Traffic Cone")) {
                    conePlacements.push_back(pendingConePos);
                }
                if (!conePlacements.empty() && ImGui::MenuItem("Remove Last Cone")) {
                    conePlacements.pop_back();
                }
                if (!conePlacements.empty() && ImGui::MenuItem("Clear All Cones")) {
                    conePlacements.clear();
                }
                ImGui::EndPopup();
            }

            ImGui::End();
        }

        // Progress Bar window
        if (replayMode) {
            ImGui::Begin("Playback Controls", nullptr, ImGuiWindowFlags_NoCollapse);
            
            // Play/Pause button
            if (ImGui::Button(csvPlayer.isPlaying() ? "Pause" : "Play")) {
                csvPlayer.togglePause();
            }
            ImGui::SameLine();
            
            // Progress bar with dragging
            float progress = csvPlayer.getProgress();
            if (ImGui::SliderFloat("##Progress", &progress, 0.0f, 1.0f, csvPlayer.getTimeString().c_str())) {
                csvPlayer.seekTo(progress);
            }

            // Playback speed control
            const char* speedLabels[] = { "1x", "2x", "4x", "8x", "16x" };
            const float speedValues[] = { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };
            float curSpeed = csvPlayer.playbackSpeed.load();
            int speedIdx = 0;
            for (int s = 0; s < 5; ++s) {
                if (std::abs(curSpeed - speedValues[s]) < 0.01f) { speedIdx = s; break; }
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            if (ImGui::Combo("Speed", &speedIdx, speedLabels, 5)) {
                csvPlayer.playbackSpeed = speedValues[speedIdx];
            }

            ImGui::End();
        }

        // Data View window
        if (replayMode) {
            ImGui::Begin("Data View", nullptr, ImGuiWindowFlags_NoCollapse);
            
            auto currentData = csvPlayer.getCurrentRowData();
            auto headers = csvPlayer.getHeaders();
            
            if (!currentData.empty() && !headers.empty()) {
                // Create a table to display the data
                if (ImGui::BeginTable("DataTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Property");
                    ImGui::TableSetupColumn("Value");
                    ImGui::TableHeadersRow();

                    for (size_t i = 0; i < headers.size() && i < currentData.size(); ++i) {
                        ImGui::TableNextRow();
                        
                        // Property column
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", headers[i].c_str());
                        
                        // Value column
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", currentData[i].c_str());
                    }
                    ImGui::EndTable();
                }
            } else {
                ImGui::Text("No data available");
            }

            // Cone list
            if (!conePlacements.empty()) {
                ImGui::Separator();
                ImGui::Text("Traffic Cones (%zu):", conePlacements.size());
                if (ImGui::BeginTable("ConeTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                    ImGui::TableSetupColumn("Latitude");
                    ImGui::TableSetupColumn("Longitude");
                    ImGui::TableHeadersRow();

                    double lat0Rad = LAT0 * DEG2RAD;
                    int removeIdx = -1;
                    for (size_t i = 0; i < conePlacements.size(); ++i) {
                        auto& cp = conePlacements[i];
                        // Reverse ENU: world x = east*0.025, world z = -north*0.025
                        double east  = cp.x / 0.025;
                        double north = -cp.z / 0.025;
                        double lat = LAT0 + north / EARTH_R / DEG2RAD;
                        double lon = LON0 + east / (EARTH_R * cos(lat0Rad)) / DEG2RAD;

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%zu", i + 1);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.8f", lat);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.8f", lon);

                        // Right-click row to remove
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            removeIdx = (int)i;
                    }
                    ImGui::EndTable();
                    if (removeIdx >= 0)
                        conePlacements.erase(conePlacements.begin() + removeIdx);
                }
            }
            
            ImGui::End();
        }

        ImGui::End(); // End the dock space window

        /* update from pose (WS or CSV) */
        {
            std::lock_guard lk(poseMtx);
            if(pose.valid && (!manualMode || replayMode)){  // Allow updates in both live and replay modes
                currentLat = pose.lat;
                currentLon = pose.lon;
                glm::dvec2 enu=llToENU(pose.lat,pose.lon);
                // Scale the ENU coordinates to match grid (0.025 units = 1 meter)
                targetModelPos.x = (float)(enu.x * 0.025);
                targetModelPos.z = (float)(-enu.y * 0.025);  // north→ -Z
                targetModelPos.y = modelPos.y;  // keep Y unchanged
                // Compass heading: 0°=North(−Z), 90°=East(+X), CW positive
                // Model natively faces +Z; at modelYaw=180° it faces −Z (North)
                // So modelYaw = 180 − headingDeg maps compass to model correctly
                float headingDeg = (float)(pose.head * 180.0 / M_PI);
                targetModelYaw = 180.0f - headingDeg;

                // Update map tiles around current position
                mapTiles.update(pose.lat, pose.lon);
            }
        }

        /* smooth interpolation for position and yaw */
        if (!manualMode || replayMode) {
            float t = glm::clamp(LERP_SPEED * deltaTime, 0.0f, 1.0f);
            // Snap instantly when user seeks
            if (csvPlayer.snapRequested.exchange(false)) {
                modelPos = targetModelPos;
                modelYaw = targetModelYaw;
                trackPoints.clear();
            } else {
                modelPos = glm::mix(modelPos, targetModelPos, t);

                // Shortest-path yaw interpolation
                float yawDiff = targetModelYaw - modelYaw;
                // Wrap to [-180, 180]
                while (yawDiff >  180.0f) yawDiff -= 360.0f;
                while (yawDiff < -180.0f) yawDiff += 360.0f;
                modelYaw += yawDiff * t;
            }

            updateCamera();
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
    delete mapTileShader;
    delete car;
    delete coneModel;

    stopWS=true;
    csvPlayer.stopAndJoin();
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
    if (glfwGetKey(window,GLFW_KEY_W)==GLFW_PRESS){ 
        modelPos += speed*carForward; 
        updateCamera();
    }
    if (glfwGetKey(window,GLFW_KEY_S)==GLFW_PRESS){ 
        modelPos -= speed*carForward; 
        updateCamera();
    }
    if (glfwGetKey(window,GLFW_KEY_A)==GLFW_PRESS){ 
        modelYaw += 90.0f*deltaTime; 
        updateCamera();
    }
    if (glfwGetKey(window,GLFW_KEY_D)==GLFW_PRESS){ 
        modelYaw -= 90.0f*deltaTime; 
        updateCamera();
    }
}
void framebuffer_size_callback(GLFWwindow*,int w,int h){ glViewport(0,0,w,h); }

void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    if(cameraLocked) return;  // Skip mouse input if camera is locked
    
    if(topDownView) {
        // Handle dragging in top-down view
        if(glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
            if(!isDragging) {
                isDragging = true;
                lastDragPos = glm::vec2(xpos, ypos);
            } else {
                // Calculate drag delta and update camera position
                glm::vec2 currentPos(xpos, ypos);
                glm::vec2 delta = currentPos - lastDragPos;
                
                // Convert screen delta to world space movement
                // Scale factor to control drag sensitivity
                const float dragSensitivity = 0.01f;
                cameraPos.x -= delta.x * dragSensitivity;
                cameraPos.z -= delta.y * dragSensitivity;  // Changed from += to -= to match mouse movement
                
                lastDragPos = currentPos;
            }
        } else {
            isDragging = false;
        }
        return;
    }
    
    // Original mouse look behavior for non-top-down view
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
    if(fov>=1.0f && fov<=90.0f) fov -= (float)yoff;
    if(fov<1.0f)  fov = 1.0f;
    if(fov>90.0f) fov = 90.0f;
}
