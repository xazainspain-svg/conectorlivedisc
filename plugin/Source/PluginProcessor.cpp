#include "PluginProcessor.h"

// Background thread: drains the FIFO and ships fixed-size UDP packets to the bridge.
// Nothing here ever blocks the audio thread.
class SendSender : public juce::Thread
{
public:
    explicit SendSender (LiveDiscordSendAudioProcessor& p) : juce::Thread ("LDSC sender"), proc (p) {}

    void run() override
    {
        std::vector<float> tmp (ldsc::kFramesPerPacket * 2);
        std::vector<juce::uint8> pkt (ldsc::kHeaderBytes + ldsc::kFramesPerPacket * 4);
        juce::uint32 seq = 0;

        while (! threadShouldExit())
        {
            if (proc.fifo.getNumReady() < ldsc::kFramesPerPacket)
            {
                wait (1);
                continue;
            }

            int s1, n1, s2, n2;
            proc.fifo.prepareToRead (ldsc::kFramesPerPacket, s1, n1, s2, n2);
            std::memcpy (tmp.data(), proc.fifoData.data() + s1 * 2, (size_t) n1 * 2 * sizeof (float));
            if (n2 > 0)
                std::memcpy (tmp.data() + n1 * 2, proc.fifoData.data() + s2 * 2, (size_t) n2 * 2 * sizeof (float));
            proc.fifo.finishedRead (n1 + n2);

            if (proc.enabledParam->load() < 0.5f)
                continue; // drain but don't send

            auto* h = pkt.data();
            write32 (h + 0, ldsc::kMagic);
            write32 (h + 4, seq++);
            write32 (h + 8, (juce::uint32) proc.currentSampleRate.load());
            write16 (h + 12, 2);
            write16 (h + 14, (juce::uint16) ldsc::kFramesPerPacket);

            auto* out = reinterpret_cast<juce::int16*> (h + ldsc::kHeaderBytes);
            for (int i = 0; i < ldsc::kFramesPerPacket * 2; ++i)
            {
                const float v = juce::jlimit (-1.0f, 1.0f, tmp[(size_t) i]);
                out[i] = (juce::int16) juce::roundToInt (v * 32767.0f);
            }

            socket.write ("127.0.0.1", (int) proc.portParam->load(), pkt.data(), (int) pkt.size());
        }
    }

private:
    static void write32 (juce::uint8* d, juce::uint32 v) { for (int i = 0; i < 4; ++i) d[i] = (juce::uint8) (v >> (8 * i)); }
    static void write16 (juce::uint8* d, juce::uint16 v) { d[0] = (juce::uint8) v; d[1] = (juce::uint8) (v >> 8); }

    LiveDiscordSendAudioProcessor& proc;
    juce::DatagramSocket socket { false };
};

LiveDiscordSendAudioProcessor::LiveDiscordSendAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout()),
      fifoData ((size_t) kFifoFrames * 2, 0.0f)
{
    enabledParam = apvts.getRawParameterValue ("enabled");
    portParam = apvts.getRawParameterValue ("port");
    sender = std::make_unique<SendSender> (*this);
    sender->startThread (juce::Thread::Priority::high);
}

LiveDiscordSendAudioProcessor::~LiveDiscordSendAudioProcessor()
{
    sender->stopThread (500);
}

juce::AudioProcessorValueTreeState::ParameterLayout LiveDiscordSendAudioProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "enabled", 1 }, "Send to Discord", true));
    l.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { "port", 1 }, "UDP Port", 1024, 65535, 9955));
    return l;
}

void LiveDiscordSendAudioProcessor::prepareToPlay (double sr, int)
{
    currentSampleRate = (int) sr;
    fifo.reset();
    setLatencySamples (0); // pure passthrough: no plugin delay compensation
}

bool LiveDiscordSendAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void LiveDiscordSendAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || n == 0)
        return;

    // Audio is left untouched (passthrough); we only copy it into the FIFO.
    int s1, n1, s2, n2;
    fifo.prepareToWrite (n, s1, n1, s2, n2); // if full, the excess is simply dropped
    const float* l = buffer.getReadPointer (0);
    const float* r = buffer.getReadPointer (1);

    for (int i = 0; i < n1; ++i) { fifoData[(size_t) (s1 + i) * 2] = l[i];      fifoData[(size_t) (s1 + i) * 2 + 1] = r[i]; }
    for (int i = 0; i < n2; ++i) { fifoData[(size_t) (s2 + i) * 2] = l[n1 + i]; fifoData[(size_t) (s2 + i) * 2 + 1] = r[n1 + i]; }
    fifo.finishedWrite (n1 + n2);
}

juce::AudioProcessorEditor* LiveDiscordSendAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void LiveDiscordSendAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void LiveDiscordSendAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LiveDiscordSendAudioProcessor();
}
