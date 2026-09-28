#include "sounddecoder.hpp"

namespace MWSound
{
    // Default readAll implementation, for decoders that can't do anything
    // better
    void SoundDecoder::readAll(std::vector<char>& output)
    {
        size_t total = output.size();
        size_t got;

        output.resize(total + 32768);
        while ((got = read(&output[total], output.size() - total)) > 0)
        {
            total += got;
            output.resize(total * 2);
        }
        output.resize(total);
    }

    const char* getSampleTypeName(SampleType type)
    {
        switch (type)
        {
            case SampleType_UInt8:
                return "U8";
            case SampleType_Int16:
                return "S16";
            case SampleType_Float32:
                return "Float32";
        }
        return "(unknown sample type)";
    }

    const char* getChannelConfigName(ChannelConfig config)
    {
        switch (config)
        {
            case ChannelConfig_Mono:
                return "Mono";
            case ChannelConfig_Stereo:
                return "Stereo";
            case ChannelConfig_Quad:
                return "Quad";
            case ChannelConfig_5point1:
                return "5.1 Surround";
            case ChannelConfig_7point1:
                return "7.1 Surround";
        }
        return "(unknown channel config)";
    }

    size_t framesToBytes(size_t frames, ChannelConfig config, SampleType type)
    {
        switch (config)
        {
            case ChannelConfig_Mono:
                frames *= 1;
                break;
            case ChannelConfig_Stereo:
                frames *= 2;
                break;
            case ChannelConfig_Quad:
                frames *= 4;
                break;
            case ChannelConfig_5point1:
                frames *= 6;
                break;
            case ChannelConfig_7point1:
                frames *= 8;
                break;
        }
        switch (type)
        {
            case SampleType_UInt8:
                frames *= 1;
                break;
            case SampleType_Int16:
                frames *= 2;
                break;
            case SampleType_Float32:
                frames *= 4;
                break;
        }
        return frames;
    }

    size_t bytesToFrames(size_t bytes, ChannelConfig config, SampleType type)
    {
        return bytes / framesToBytes(1, config, type);
    }

}
