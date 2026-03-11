#include <iostream>
#include <optional>
#include <stdexcept>

#include <JuceHeader.h>

#include "AudioUtils.h"
#include "BasicPitch.h"
#include "MidiFileWriter.h"

namespace
{
struct CliOptions {
    juce::File inputFile;
    juce::File outputFile;
    double bpm = 120.0;
    int timeSignatureNum = 4;
    int timeSignatureDenom = 4;
    float noteSensitivity = 0.7f;
    float splitSensitivity = 0.5f;
    float minNoteDurationMs = 125.0f;
    PitchBendModes pitchBendMode = NoPitchBend;
};

void printUsage(const char* executableName)
{
    std::cout << "Usage: " << executableName << " <input-audio> <output-midi> [options]\n"
              << "\n"
              << "Options:\n"
              << "  --note-sensitivity <0.05-0.95>      Default: 0.7\n"
              << "  --split-sensitivity <0.05-0.95>     Default: 0.5\n"
              << "  --min-note-duration-ms <35-580>     Default: 125\n"
              << "  --pitch-bend <none|single>          Default: none\n"
              << "  --bpm <positive-number>             Default: 120\n"
              << "  --time-signature <num/denom>        Default: 4/4\n"
              << "  --help                              Show this help\n"
              << "\n"
              << "Supported input formats: .wav, .aiff, .flac, .mp3, .ogg\n";
}

std::optional<double> parseDouble(const juce::String& rawValue)
{
    try {
        size_t parsedChars = 0;
        const auto parsed = std::stod(rawValue.toStdString(), &parsedChars);
        if (parsedChars != static_cast<size_t>(rawValue.getNumBytesAsUTF8())) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int> parseInt(const juce::String& rawValue)
{
    try {
        size_t parsedChars = 0;
        const auto parsed = std::stoi(rawValue.toStdString(), &parsedChars);
        if (parsedChars != static_cast<size_t>(rawValue.getNumBytesAsUTF8())) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool parseTimeSignature(const juce::String& rawValue, int& outNumerator, int& outDenominator)
{
    const auto separatorIdx = rawValue.indexOfChar('/');
    if (separatorIdx <= 0 || separatorIdx >= rawValue.length() - 1) {
        return false;
    }

    const auto numerator = parseInt(rawValue.substring(0, separatorIdx).trim());
    const auto denominator = parseInt(rawValue.substring(separatorIdx + 1).trim());

    if (!numerator.has_value() || !denominator.has_value()) {
        return false;
    }

    if (*numerator <= 0 || *denominator <= 0) {
        return false;
    }

    outNumerator = *numerator;
    outDenominator = *denominator;
    return true;
}

juce::AudioBuffer<float> downmixToMono(const juce::AudioBuffer<float>& inputBuffer)
{
    juce::AudioBuffer<float> monoBuffer;
    monoBuffer.setSize(1, inputBuffer.getNumSamples());

    monoBuffer.copyFrom(0, 0, inputBuffer, 0, 0, inputBuffer.getNumSamples());

    for (int channel = 1; channel < inputBuffer.getNumChannels(); channel++) {
        monoBuffer.addFrom(0, 0, inputBuffer, channel, 0, inputBuffer.getNumSamples());
    }

    monoBuffer.applyGain(1.0f / static_cast<float>(inputBuffer.getNumChannels()));
    return monoBuffer;
}

std::optional<CliOptions> parseArguments(int argc, char* argv[], bool& outShowedHelp)
{
    outShowedHelp = false;

    if (argc <= 1) {
        printUsage(argv[0]);
        return std::nullopt;
    }

    CliOptions options;
    juce::StringArray positionalArgs;

    for (int i = 1; i < argc; i++) {
        const juce::String arg(argv[i]);

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            outShowedHelp = true;
            return std::nullopt;
        }

        if (!arg.startsWithChar('-')) {
            positionalArgs.add(arg);
            continue;
        }

        if (i + 1 >= argc) {
            std::cerr << "Missing value for option: " << arg << std::endl;
            return std::nullopt;
        }

        const juce::String value(argv[++i]);

        if (arg == "--note-sensitivity") {
            auto parsed = parseDouble(value);
            if (!parsed.has_value()) {
                std::cerr << "Invalid note sensitivity: " << value << std::endl;
                return std::nullopt;
            }
            options.noteSensitivity = static_cast<float>(*parsed);

        } else if (arg == "--split-sensitivity") {
            auto parsed = parseDouble(value);
            if (!parsed.has_value()) {
                std::cerr << "Invalid split sensitivity: " << value << std::endl;
                return std::nullopt;
            }
            options.splitSensitivity = static_cast<float>(*parsed);

        } else if (arg == "--min-note-duration-ms") {
            auto parsed = parseDouble(value);
            if (!parsed.has_value()) {
                std::cerr << "Invalid minimum note duration: " << value << std::endl;
                return std::nullopt;
            }
            options.minNoteDurationMs = static_cast<float>(*parsed);

        } else if (arg == "--bpm") {
            auto parsed = parseDouble(value);
            if (!parsed.has_value()) {
                std::cerr << "Invalid bpm: " << value << std::endl;
                return std::nullopt;
            }
            options.bpm = *parsed;

        } else if (arg == "--time-signature") {
            if (!parseTimeSignature(value, options.timeSignatureNum, options.timeSignatureDenom)) {
                std::cerr << "Invalid time signature: " << value << std::endl;
                return std::nullopt;
            }

        } else if (arg == "--pitch-bend") {
            const auto normalizedValue = value.toLowerCase();
            if (normalizedValue == "none") {
                options.pitchBendMode = NoPitchBend;
            } else if (normalizedValue == "single") {
                options.pitchBendMode = SinglePitchBend;
            } else {
                std::cerr << "Invalid pitch bend mode: " << value << std::endl;
                return std::nullopt;
            }

        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            return std::nullopt;
        }
    }

    if (positionalArgs.size() != 2) {
        std::cerr << "Expected an input audio path and an output midi path." << std::endl;
        return std::nullopt;
    }

    options.inputFile = juce::File(positionalArgs[0]);
    options.outputFile = juce::File(positionalArgs[1]);

    if (options.noteSensitivity < 0.05f || options.noteSensitivity > 0.95f) {
        std::cerr << "--note-sensitivity must be between 0.05 and 0.95." << std::endl;
        return std::nullopt;
    }

    if (options.splitSensitivity < 0.05f || options.splitSensitivity > 0.95f) {
        std::cerr << "--split-sensitivity must be between 0.05 and 0.95." << std::endl;
        return std::nullopt;
    }

    if (options.minNoteDurationMs < 35.0f || options.minNoteDurationMs > 580.0f) {
        std::cerr << "--min-note-duration-ms must be between 35 and 580." << std::endl;
        return std::nullopt;
    }

    if (options.bpm <= 0.0) {
        std::cerr << "--bpm must be positive." << std::endl;
        return std::nullopt;
    }

    return options;
}

int run(const CliOptions& options)
{
    if (!options.inputFile.existsAsFile()) {
        std::cerr << "Input audio file does not exist: " << options.inputFile.getFullPathName() << std::endl;
        return 1;
    }

    auto outputParentDir = options.outputFile.getParentDirectory();
    if (!outputParentDir.isDirectory()) {
        auto result = outputParentDir.createDirectory();
        if (result.failed()) {
            std::cerr << "Failed to create output directory: " << outputParentDir.getFullPathName() << std::endl;
            return 1;
        }
    }

    if (options.outputFile.existsAsFile() && !options.outputFile.deleteFile()) {
        std::cerr << "Failed to overwrite output file: " << options.outputFile.getFullPathName() << std::endl;
        return 1;
    }

    juce::AudioBuffer<float> sourceAudio;
    double sourceSampleRate = 0.0;

    if (!AudioUtils::loadAudioFile(options.inputFile, sourceAudio, sourceSampleRate)) {
        std::cerr << "Failed to load audio file: " << options.inputFile.getFullPathName() << std::endl;
        return 1;
    }

    if (sourceAudio.getNumChannels() == 0 || sourceAudio.getNumSamples() == 0) {
        std::cerr << "Audio file is empty: " << options.inputFile.getFullPathName() << std::endl;
        return 1;
    }

    auto monoAudio = downmixToMono(sourceAudio);

    juce::AudioBuffer<float> downsampledAudio;
    AudioUtils::resampleBuffer(monoAudio, downsampledAudio, sourceSampleRate, BASIC_PITCH_SAMPLE_RATE);

    BasicPitch basicPitch;
    basicPitch.setParameters(options.noteSensitivity, options.splitSensitivity, options.minNoteDurationMs);
    basicPitch.transcribeToMIDI(downsampledAudio.getWritePointer(0), downsampledAudio.getNumSamples());

    std::vector<Notes::Event> noteEvents(
        basicPitch.getNoteEvents().begin(), basicPitch.getNoteEvents().end());

    Notes::dropOverlappingPitchBends(noteEvents);
    Notes::mergeOverlappingNotesWithSamePitch(noteEvents);

    TimeQuantizeOptions::TimeQuantizeInfo exportInfo;
    exportInfo.bpm = options.bpm;
    exportInfo.timeSignatureNum = options.timeSignatureNum;
    exportInfo.timeSignatureDenom = options.timeSignatureDenom;

    MidiFileWriter midiFileWriter;
    if (!midiFileWriter.writeMidiFile(
            noteEvents, options.outputFile, exportInfo, options.bpm, options.pitchBendMode)) {
        std::cerr << "Failed to write midi file: " << options.outputFile.getFullPathName() << std::endl;
        return 1;
    }

    std::cout << "Transcribed " << noteEvents.size() << " notes to " << options.outputFile.getFullPathName()
              << std::endl;
    return 0;
}
} // namespace

int main(int argc, char* argv[])
{
    bool showedHelp = false;
    auto options = parseArguments(argc, argv, showedHelp);
    if (!options.has_value()) {
        if (!showedHelp && argc > 1) {
            std::cout << std::endl;
            printUsage(argv[0]);
        }
        return showedHelp ? 0 : 1;
    }

    return run(*options);
}
