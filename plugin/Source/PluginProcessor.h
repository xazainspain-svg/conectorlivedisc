#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

// Packet sent over UDP (little endian):
//   u32 magic 'LDSC' | u32 seq | u32 sampleRate | u16 channels(=2) | u16 frames | int16[frames*2]
namespace ldsc
{
constexpr juce::uint32 kMagic = 0x4353444c; // "LDSC" little endian
constexpr int kHeaderBytes = 16;
constexpr int kFramesPerPacket = 240;       // 5 ms @ 48 kHz, 960 bytes payload (< MTU)
}

class SendSender;

class LiveDiscordSendAudioProcessor : public juce::AudioProcessor
{
public:
    LiveDiscordSendAudioProcessor();
    ~LiveDiscordSendAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Live Discord Send"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // Lock-free SPSC FIFO: audio thread -> sender thread (interleaved stereo float)
    static constexpr int kFifoFrames = 1 << 15;
    juce::AbstractFifo fifo { kFifoFrames };
    std::vector<float> fifoData;
    std::atomic<int> currentSampleRate { 48000 };

    std::unique_ptr<SendSender> sender;
    std::atomic<float>* enabledParam = nullptr;
    std::atomic<float>* portParam = nullptr;

    friend class SendSender;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveDiscordSendAudioProcessor)
};
