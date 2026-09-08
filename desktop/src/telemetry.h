#ifndef VMP_TELEMETRY_H
#define VMP_TELEMETRY_H

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <numeric>
#include <algorithm>

struct BenchmarkSample {
    double timestamp_sec;
    double fps;
    double decode_time_ms;
};

class TelemetryExporter {
public:
    TelemetryExporter();
    ~TelemetryExporter();

    void record_sample(double timestamp_sec, double fps, double decode_time_ms);
    bool export_json(const std::string& output_filepath, const std::string& codec_name, 
                     int width, int height, const std::string& hw_accel);

private:
    std::vector<BenchmarkSample> samples;
};

#endif // VMP_TELEMETRY_H
