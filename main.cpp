#include "screen_capture.h"
#include <signal.h>  // For signal handling (Ctrl+C)

// Global pointer for signal handler
ScreenCaptureEncoder* g_encoder = nullptr;

// Signal handler for graceful shutdown (Ctrl+C)
void SignalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", stopping capture..." << std::endl;
    if (g_encoder) {
        g_encoder->Stop();  // Stop capture gracefully
    }
    exit(0);  // Exit program
}

int main(int argc, char* argv[]) {
    std::cout << "=== Screen Capture & Encoder for Cloud Gaming ===" << std::endl;
    std::cout << "This program captures screen and audio, encodes to H.264/AAC," << std::endl;
    std::cout << "and sends to Go WebRTC process via named pipe." << std::endl;
    std::cout << std::endl;
    
    // Parse command line arguments
    int width = 1920;      // Default width
    int height = 1080;     // Default height
    int fps = 60;          // Default FPS
    std::wstring pipe_name = L"\\\\.\\pipe\\CloudGameCapture";  // Default pipe name
    
    // Simple argument parsing
    // Usage: program.exe [width] [height] [fps] [pipe_name]
    if (argc > 1) {
        width = std::stoi(argv[1]);   // Convert string to int
    }
    if (argc > 2) {
        height = std::stoi(argv[2]);
    }
    if (argc > 3) {
        fps = std::stoi(argv[3]);
    }
    if (argc > 4) {
        // Convert narrow string to wide string
        std::string narrow_pipe(argv[4]);
        pipe_name = std::wstring(narrow_pipe.begin(), narrow_pipe.end());
    }
    
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Resolution: " << width << "x" << height << std::endl;
    std::cout << "  FPS: " << fps << std::endl;
    std::wcout << L"  Pipe Name: " << pipe_name << std::endl;
    std::cout << std::endl;
    
    // Create encoder instance
    ScreenCaptureEncoder encoder;
    g_encoder = &encoder;  // Set global pointer for signal handler
    
    // Register signal handler for Ctrl+C
    signal(SIGINT, SignalHandler);   // Ctrl+C
    signal(SIGTERM, SignalHandler);  // Termination signal
    
    std::cout << "Initializing capture system..." << std::endl;
    
    // Initialize encoder
    if (!encoder.Initialize(width, height, fps, pipe_name)) {
        std::cerr << "Failed to initialize encoder!" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Common issues:" << std::endl;
        std::cerr << "  1. Another program is already capturing (close OBS, Discord, etc.)" << std::endl;
        std::cerr << "  2. Running in a game with anti-cheat that blocks capture" << std::endl;
        std::cerr << "  3. GPU doesn't support hardware encoding (rare)" << std::endl;
        std::cerr << "  4. Go process not connected to pipe yet" << std::endl;
        return 1;
    }
    
    std::cout << std::endl;
    std::cout << "Starting capture..." << std::endl;
    
    // Start capture threads
    if (!encoder.Start()) {
        std::cerr << "Failed to start encoder!" << std::endl;
        return 1;
    }
    
    std::cout << std::endl;
    std::cout << "Capture running! Press Ctrl+C to stop." << std::endl;
    std::cout << "Video frames are being captured from GPU and encoded to H.264" << std::endl;
    std::cout << "Audio is being captured from speakers and encoded to AAC" << std::endl;
    std::cout << "Encoded data is being sent to Go process via named pipe" << std::endl;
    std::cout << std::endl;
    
    // Keep main thread alive
    // The actual work happens in the capture threads
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // You could add status updates here
        // For example, print frames captured, bitrate, etc.
    }
    
    // This is never reached in normal operation (Ctrl+C calls signal handler)
    encoder.Stop();
    return 0;
}
