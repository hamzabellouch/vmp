#include "telemetry.h"

TelemetryExporter::TelemetryExporter() {}
TelemetryExporter::~TelemetryExporter() {}

void TelemetryExporter::record_sample(double timestamp_sec, double fps, double decode_time_ms) {
    samples.push_back({timestamp_sec, fps, decode_time_ms});
}

bool TelemetryExporter::export_json(const std::string& output_filepath, const std::string& codec_name, 
                                   int width, int height, const std::string& hw_accel) {
    if (samples.empty()) {
        std::cerr << "[VMP Telemetry] No samples recorded to export." << std::endl;
        return false;
    }

    double total_fps = 0.0;
    double max_fps = 0.0;
    double min_fps = 999999.0;
    double total_decode_time = 0.0;

    std::vector<double> fps_list;

    for (const auto& s : samples) {
        total_fps += s.fps;
        total_decode_time += s.decode_time_ms;
        max_fps = std::max(max_fps, s.fps);
        min_fps = std::min(min_fps, s.fps);
        fps_list.push_back(s.fps);
    }

    double avg_fps = total_fps / samples.size();
    double avg_decode_time = total_decode_time / samples.size();

    // Calculate 1% Low FPS metric
    std::sort(fps_list.begin(), fps_list.end());
    size_t low1_index = std::max(size_t(0), size_t(fps_list.size() * 0.01));
    double low_1percent_fps = fps_list[low1_index];

    std::ofstream out(output_filepath);
    if (!out.is_open()) {
        std::cerr << "[VMP Telemetry] Failed to write report file: " << output_filepath << std::endl;
        return false;
    }

    out << "{\n";
    out << "  \"engine\": \"VMP Ultra-Native Engine\",\n";
    out << "  \"media_info\": {\n";
    out << "    \"resolution\": \"" << width << "x" << height << "\",\n";
    out << "    \"codec\": \"" << codec_name << "\",\n";
    out << "    \"hardware_acceleration\": \"" << hw_accel << "\"\n";
    out << "  },\n";
    out << "  \"benchmark_summary\": {\n";
    out << "    \"total_samples\": " << samples.size() << ",\n";
    out << "    \"average_fps\": " << avg_fps << ",\n";
    out << "    \"max_fps\": " << max_fps << ",\n";
    out << "    \"min_fps\": " << min_fps << ",\n";
    out << "    \"low_1percent_fps\": " << low_1percent_fps << ",\n";
    out << "    \"average_decode_time_ms\": " << avg_decode_time << "\n";
    out << "  }\n";
    out << "}\n";

    out.close();

    std::cout << "\n[VMP Telemetry] Benchmark Report successfully exported to: " << output_filepath << std::endl;
    std::cout << " -> Avg FPS: " << avg_fps << " | 1% Low FPS: " << low_1percent_fps << " | Max FPS: " << max_fps << std::endl;

    return true;
}
