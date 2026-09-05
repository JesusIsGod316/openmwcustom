#include "sensormanager.hpp"

#include <SDL3/SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

namespace
{
    SDL_DisplayID configuredDisplayId()
    {
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        SDL_DisplayID id = 0;
        const int screen = Settings::video().mScreen;
        if (displays && screen >= 0 && screen < count)
            id = displays[screen];
        SDL_free(displays);
        return id ? id : SDL_GetPrimaryDisplay();
    }
}

namespace MWInput
{
    SensorManager::SensorManager()
        : mRotation()
        , mGyroValues()
        , mGyroUpdateTimer(0.f)
        , mGyroscope(nullptr)
    {
        init();
    }

    void SensorManager::init()
    {
        correctGyroscopeAxes();
        updateSensors();
    }

    SensorManager::~SensorManager()
    {
        if (mGyroscope != nullptr)
        {
            SDL_CloseSensor(mGyroscope);
            mGyroscope = nullptr;
        }
    }

    void SensorManager::correctGyroscopeAxes()
    {
        if (!Settings::input().mEnableGyroscope)
            return;

        mRotation = osg::Matrixf::identity();

        float angle = 0;
        const SDL_DisplayID display = configuredDisplayId();
        const SDL_DisplayOrientation currentOrientation
            = display ? SDL_GetCurrentDisplayOrientation(display) : SDL_ORIENTATION_UNKNOWN;
        switch (currentOrientation)
        {
            case SDL_ORIENTATION_UNKNOWN:
            case SDL_ORIENTATION_LANDSCAPE:
                break;
            case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
                angle = osg::PIf;
                break;
            case SDL_ORIENTATION_PORTRAIT:
                angle = -0.5 * osg::PIf;
                break;
            case SDL_ORIENTATION_PORTRAIT_FLIPPED:
                angle = 0.5 * osg::PIf;
                break;
        }

        mRotation.makeRotate(angle, osg::Vec3f(0, 0, 1));
    }

    void SensorManager::updateSensors()
    {
        if (Settings::input().mEnableGyroscope)
        {
            int count = 0;
            SDL_SensorID* sensors = SDL_GetSensors(&count);
            for (int i = 0; sensors && i < count; ++i)
            {
                const SDL_SensorID id = sensors[i];
                if (SDL_GetSensorTypeForID(id) != SDL_SENSOR_GYRO)
                    continue;

                if (mGyroscope != nullptr)
                {
                    SDL_CloseSensor(mGyroscope);
                    mGyroscope = nullptr;
                    mGyroUpdateTimer = 0.f;
                }

                SDL_Sensor* sensor = SDL_OpenSensor(id);
                if (sensor == nullptr)
                {
                    const char* name = SDL_GetSensorNameForID(id);
                    Log(Debug::Error) << "Couldn't open sensor " << (name ? name : "<unnamed>") << ": "
                                      << SDL_GetError();
                }
                else
                {
                    mGyroscope = sensor;
                    break;
                }
            }
            SDL_free(sensors);
        }
        else if (mGyroscope != nullptr)
        {
            SDL_CloseSensor(mGyroscope);
            mGyroscope = nullptr;
            mGyroUpdateTimer = 0.f;
        }
    }

    void SensorManager::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        for (const auto& setting : changed)
        {
            if (setting.first == "Input" && setting.second == "enable gyroscope")
                init();
        }
    }

    void SensorManager::displayOrientationChanged()
    {
        correctGyroscopeAxes();
    }

    void SensorManager::sensorUpdated(const SDL_SensorEvent& arg)
    {
        if (!Settings::input().mEnableGyroscope)
            return;

        SDL_Sensor* sensor = SDL_GetSensorFromID(arg.which);
        if (!sensor)
        {
            Log(Debug::Info) << "Couldn't get sensor for sensor event";
            return;
        }

        switch (SDL_GetSensorType(sensor))
        {
            case SDL_SENSOR_ACCEL:
                break;
            case SDL_SENSOR_GYRO:
            {
                osg::Vec3f gyro(arg.data[0], arg.data[1], arg.data[2]);
                mGyroValues = mRotation * gyro;
                mGyroUpdateTimer = 0.f;
                break;
            }
            default:
                break;
        }
    }

    void SensorManager::update(float dt)
    {
        mGyroUpdateTimer += dt;
        if (mGyroUpdateTimer > 0.5f)
        {
            mGyroValues = osg::Vec3f();
            mGyroUpdateTimer = 0.f;
        }
    }

    bool SensorManager::isGyroAvailable() const
    {
        return mGyroscope != nullptr;
    }

    std::array<float, 3> SensorManager::getGyroValues() const
    {
        return { mGyroValues.x(), mGyroValues.y(), mGyroValues.z() };
    }
}
