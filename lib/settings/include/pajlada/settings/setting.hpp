#pragma once

#include <rapidjson/document.h>

#include <iostream>
#include <memory>
#include <mutex>
#include <pajlada/settings/common.hpp>
#include <pajlada/settings/equal.hpp>
#include <pajlada/settings/settingdata.hpp>
#include <pajlada/settings/settingmanager.hpp>
#include <pajlada/signals.hpp>
#include <string>
#include <type_traits>

namespace pajlada::Settings {

namespace detail {

inline SignalArgs
onConnectArgs()
{
    static SignalArgs a = []() {
        SignalArgs v;
        v.source = SignalArgs::Source::OnConnect;

        return v;
    }();

    return a;
}

template <typename Manager>
concept IsScopedList =
    std::is_same_v<typename Manager::value_type, Signals::ScopedConnection>;

template <typename Manager>
concept IsUpScopedList =
    std::is_same_v<typename Manager::value_type,
                   std::unique_ptr<Signals::ScopedConnection>>;

template <typename Manager>
void connectToManager(Manager &manager, Signals::Connection &&connection);

template <IsScopedList Manager>
void
connectToManager(Manager &manager, Signals::Connection &&connection)
{
    manager.emplace_back(std::move(connection));
}

template <IsUpScopedList Manager>
void
connectToManager(Manager &manager, Signals::Connection &&connection)
{
    manager.emplace_back(
        std::make_unique<Signals::ScopedConnection>(std::move(connection)));
}

template <>
void connectToManager<Signals::ScopedConnection>(
    Signals::ScopedConnection &manager, Signals::Connection &&connection);

template <>
void connectToManager<Signals::SignalHolder>(Signals::SignalHolder &manager,
                                             Signals::Connection &&connection);

}  // namespace detail

// A default value passed to a setting is only local to this specific instance of the setting
// it is never shared between other settings at the same path
template <typename Type>
class Setting
{
    const std::string path;

public:
    explicit Setting(const std::string &_path,
                     SettingOption _options = SettingOption::Default,
                     std::shared_ptr<SettingManager> instance = nullptr)
        : path(_path)
        , data(SettingManager::getSetting(_path, instance))
        , options(_options)
    {
    }

    explicit Setting(const char *_path,
                     SettingOption _options = SettingOption::Default,
                     std::shared_ptr<SettingManager> instance = nullptr)
        : path(_path)
        , data(SettingManager::getSetting(_path, instance))
        , options(_options)
    {
    }

    explicit Setting(const std::string &_path, Type _defaultValue,
                     SettingOption _options = SettingOption::Default,
                     std::shared_ptr<SettingManager> instance = nullptr)
        : path(_path)
        , data(SettingManager::getSetting(_path, instance))
        , options(_options)
        , defaultValue(std::move(_defaultValue))
    {
    }

    explicit Setting(const char *_path, Type _defaultValue,
                     SettingOption _options = SettingOption::Default,
                     std::shared_ptr<SettingManager> instance = nullptr)
        : path(_path)
        , data(SettingManager::getSetting(_path, instance))
        , options(_options)
        , defaultValue(std::move(_defaultValue))
    {
    }

    explicit Setting(const std::string &_path,
                     std::shared_ptr<SettingManager> instance)
        : path(_path)
        , data(SettingManager::getSetting(_path, instance))
    {
    }

    explicit Setting(const char *_path,
                     std::shared_ptr<SettingManager> instance)
        : path(_path)
        , data(SettingManager::getSetting(_path, instance))
    {
    }

    explicit Setting(const std::string &_path, Type _defaultValue,
                     std::shared_ptr<SettingManager> instance)
        : path(_path)
        , data(SettingManager::getSetting(_path, instance))
        , defaultValue(std::move(_defaultValue))
    {
    }

    explicit Setting(const char *_path, Type _defaultValue,
                     std::shared_ptr<SettingManager> instance)
        : path(_path)
        , data(SettingManager::getSetting(_path, instance))
        , defaultValue(std::move(_defaultValue))
    {
    }

    // Copy constructor
    Setting(const Setting &other)
        : path(other.path)
        , data(other.data)
        , options(other.options)
        , defaultValue(other.defaultValue)
        , value(other.value)
        , updateIteration(other.updateIteration)
    {
        // managedConnections is not copied on purpose
        // valueMutex is not copied on purpose
    }

    inline bool
    optionEnabled(SettingOption option) const
    {
        return (this->options & option) == option;
    }

    ~Setting() = default;

    bool
    isValid() const
    {
        return !this->data.expired();
    }

    const std::string &
    getPath() const
    {
        return this->path;
    }

    Type
    getValueCopy() const
    {
        std::unique_lock<std::mutex> lock(this->valueMutex);

        auto lockedSetting = this->data.lock();

        if (!lockedSetting) {
            if (this->value) {
                return *this->value;
            }

            return this->defaultValue;
        }

        auto currentUpdateIteration = lockedSetting->getUpdateIteration();
        if (this->updateIteration != currentUpdateIteration) {
            this->updateIteration = currentUpdateIteration;

            auto p = lockedSetting->template unmarshal<Type>();
            if (p) {
                this->value = std::move(p);
            }
        }

        if (this->value) {
            return *this->value;
        }

        return this->defaultValue;
    }

    const Type &
    getValue() const
    {
        std::unique_lock<std::mutex> lock(this->valueMutex);

        auto lockedSetting = this->data.lock();

        if (!lockedSetting) {
            if (this->value) {
                return *this->value;
            }

            return this->defaultValue;
        }

        auto currentUpdateIteration = lockedSetting->getUpdateIteration();
        if (this->updateIteration == currentUpdateIteration) {
            // Value hasn't been updated
            if (this->value) {
                return *this->value;
            }

            return this->defaultValue;
        }
        this->updateIteration = currentUpdateIteration;

        auto p = lockedSetting->template unmarshal<Type>();
        if (p) {
            this->value = std::forward<decltype(p)>(p);
        }

        if (this->value) {
            return *this->value;
        }

        return this->defaultValue;
    }

    template <typename T = Type,
              typename = std::enable_if_t<is_stl_container<T>::value>>
    const Type &
    getArray() const
    {
        auto lockedSetting = this->data.lock();

        if (!lockedSetting) {
            if (this->defaultValue) {
                return *this->defaultValue;
            }

            return this->value;
        }

        auto p = lockedSetting->template unmarshal<Type>();
        if (p.second) {
            return p.first;
        }

        if (this->defaultValue) {
            return *this->defaultValue;
        }

        return p.first;
    }

    // Implement vector helper stuff
    template <typename T = Type,
              typename = std::enable_if_t<is_stl_container<T>::value>>
    void
    push_back(typename T::value_type newItem, SignalArgs &&args = SignalArgs())
    {
        // TODO(pajlada): refresh this->value first?
        this->valueMutex.lock();
        if (!this->value) {
            this->value = Type{};
        }

        (*this->value).push_back(std::move(newItem));
        auto copy = *this->value;
        this->valueMutex.unlock();
        this->updateValue(copy, std::move(args));
    }

    template <typename T = Type,
              typename = std::enable_if_t<is_stl_container<T>::value>>
    void
    removeByValue(const typename T::value_type &key,
                  SignalArgs &&args = SignalArgs())
    {
        // TODO(pajlada): refresh this->value first?

        T copy;

        {
            std::unique_lock<std::mutex> lock(this->valueMutex);
            if (!this->value) {
                return;
            }

            copy = (*this->value);
        }

        auto valueSize = copy.size();

        copy.erase(std::remove(copy.begin(), copy.end(), key), copy.end());

        if (copy.size() == valueSize) {
            // nothing was removed
            return;
        }

        {
            this->valueMutex.lock();
            this->value = copy;
            this->valueMutex.unlock();
            this->updateValue(copy, std::move(args));
        }
    }

private:
    bool
    updateValue(const Type &newValue, SignalArgs &&args)
    {
        if (this->optionEnabled(SettingOption::DoNotWriteToJSON)) {
            args.writeToFile = false;
        }

        auto lockedSetting = this->data.lock();

        if (lockedSetting) {
            if (args.source == SignalArgs::Source::Unset) {
                args.source = SignalArgs::Source::Setter;
            }
            return lockedSetting->marshal(newValue, std::move(args));
        }

        return false;
    }

public:
    bool
    setValue(const Type &newValue, SignalArgs &&args = SignalArgs())
    {
        if (this->optionEnabled(SettingOption::CompareBeforeSet)) {
            args.compareBeforeSet = true;
        }

        {
            std::unique_lock<std::mutex> lock(this->valueMutex);
            this->value = newValue;
        }

        if (this->optionEnabled(SettingOption::DoNotWriteToJSON)) {
            args.writeToFile = false;
        }

        auto lockedSetting = this->data.lock();

        if (lockedSetting) {
            if (args.source == SignalArgs::Source::Unset) {
                args.source = SignalArgs::Source::Setter;
            }
            return lockedSetting->marshal(newValue, std::move(args));
        }

        return false;
    }

    Setting &
    operator=(const Type &newValue)
    {
        this->setValue(newValue);

        return *this;
    }

    template <typename T2>
    Setting &
    operator=(const T2 &newValue)
    {
        this->setValue(Type(newValue));

        return *this;
    }

    Setting &
    operator=(Type &&newValue)
    {
        this->setValue(std::move(newValue));

        return *this;
    }

    bool
    operator==(const Type &rhs) const
    {
        assert(this->isValid());

        return this->getValue() == rhs;
    }

    bool
    operator!=(const Type &rhs) const
    {
        assert(this->isValid());

        return this->getValue() != rhs;
    }

    operator Type() const
    {
        assert(this->isValid());

        return this->getValue();
    }

    void
    resetToDefaultValue(SignalArgs &&args = SignalArgs())
    {
        this->setValue(this->defaultValue, std::move(args));
    }

    void
    setDefaultValue(const Type &newDefaultValue)
    {
        this->defaultValue = newDefaultValue;
    }

    Type
    getDefaultValue() const
    {
        return this->defaultValue;
    }

    // Returns true if the current value is the same as the default value
    // std::any cannot be properly compared
    bool
    isDefaultValue() const
    {
        return IsEqual<Type>::get(this->getValue(), this->getDefaultValue());
    }

    /// Remove will invalidate this setting and all other settings that point at the same path
    /// If the setting is an object or array, any child settings will also be invalidated
    /// the remove function handles the exception handling in case this setting is already invalid
    ///
    /// Where possible, try to use SettingManager::removeSetting instead
    [[deprecated("Prefer SettingManager::removeSetting instead")]]
    bool
    remove()
    {
        return SettingManager::gRemoveSetting(this->getPath());
    }

    int
    getUpdateIteration() const
    {
        std::lock_guard guard(this->valueMutex);
        return this->updateIteration;
    }

private:
    std::weak_ptr<SettingData> data;
    SettingOption options = SettingOption::Default;
    Type defaultValue{};

    // These are mutable because they can be modified from the "getValue" function
    mutable std::mutex valueMutex;
    mutable std::optional<Type> value;
    mutable int updateIteration = -1;

public:
    std::weak_ptr<SettingData>
    getData()
    {
        return this->data;
    }

    // ConnectJSON: Connect with rapidjson::Value and SignalArgs as arguments
    // No deserialization is made by the setting
    void
    connectJSON(
        std::function<void(const rapidjson::Value &, const SignalArgs &)> func,
        bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(func);

        if (autoInvoke) {
            auto ptr = lockedSetting->unmarshalJSON();
            rapidjson::Document d;
            if (ptr != nullptr) {
                d.CopyFrom(*ptr, d.GetAllocator());
            }
            lockedSetting->updated.invoke(d, detail::onConnectArgs());
        }

        this->managedConnections.emplace_back(
            std::make_unique<Signals::ScopedConnection>(std::move(connection)));
    }

    template <typename ConnectionManager>
    void
    connectJSON(
        std::function<void(const rapidjson::Value &, const SignalArgs &)> func,
        ConnectionManager &userDefinedManagedConnections,
        bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(func);

        if (autoInvoke) {
            auto ptr = lockedSetting->unmarshalJSON();
            rapidjson::Document d;
            if (ptr != nullptr) {
                d.CopyFrom(*ptr, d.GetAllocator());
            }
            lockedSetting->updated.invoke(d, detail::onConnectArgs());
        }

        detail::connectToManager(userDefinedManagedConnections,
                                 std::move(connection));
    }

    // Connect: Value and SignalArgs
    void
    connect(std::function<void(const Type &, const SignalArgs &)> func,
            bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(
            [=](const rapidjson::Value &value, const SignalArgs &args) {
                func(Deserialize<Type>::get(value), args);  //
            });

        if (autoInvoke) {
            func(this->getValue(), detail::onConnectArgs());
        }

        this->managedConnections.emplace_back(
            std::make_unique<Signals::ScopedConnection>(std::move(connection)));
    }

    template <typename ConnectionManager>
    void
    connect(std::function<void(const Type &, const SignalArgs &)> func,
            ConnectionManager &userDefinedManagedConnections,
            bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(
            [=](const rapidjson::Value &value, const SignalArgs &args) {
                func(Deserialize<Type>::get(value), args);  //
            });

        if (autoInvoke) {
            func(this->getValue(), detail::onConnectArgs());
        }

        detail::connectToManager(userDefinedManagedConnections,
                                 std::move(connection));
    }

    // Connect: Value
    void
    connect(std::function<void(const Type &)> func, bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(
            [=](const rapidjson::Value &value, const SignalArgs &) {
                func(Deserialize<Type>::get(value));  //
            });

        if (autoInvoke) {
            func(this->getValue());
        }

        this->managedConnections.emplace_back(
            std::make_unique<Signals::ScopedConnection>(std::move(connection)));
    }

    template <typename ConnectionManager>
    void
    connect(std::function<void(const Type &)> func,
            ConnectionManager &userDefinedManagedConnections,
            bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(
            [=](const rapidjson::Value &value, const SignalArgs &) {
                func(Deserialize<Type>::get(value));  //
            });

        if (autoInvoke) {
            func(this->getValue());
        }

        detail::connectToManager(userDefinedManagedConnections,
                                 std::move(connection));
    }

    // Connect: no args
    void
    connect(std::function<void()> func, bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(
            [=](const rapidjson::Value &, const SignalArgs &) {
                func();  //
            });

        if (autoInvoke) {
            func();
        }

        this->managedConnections.emplace_back(
            std::make_unique<Signals::ScopedConnection>(std::move(connection)));
    }

    template <typename ConnectionManager>
    void
    connect(std::function<void()> func,
            ConnectionManager &userDefinedManagedConnections,
            bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(
            [=](const rapidjson::Value &, const SignalArgs &) {
                func();  //
            });

        if (autoInvoke) {
            func();
        }

        detail::connectToManager(userDefinedManagedConnections,
                                 std::move(connection));
    }

    // ConnectSimple: Signal args only
    void
    connectSimple(std::function<void(const SignalArgs &)> func,
                  bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(
            [=](const rapidjson::Value &, const SignalArgs &args) {
                func(args);  //
            });

        if (autoInvoke) {
            func(detail::onConnectArgs());
        }

        this->managedConnections.emplace_back(
            std::make_unique<Signals::ScopedConnection>(std::move(connection)));
    }

    template <typename ConnectionManager>
    void
    connectSimple(std::function<void(const SignalArgs &)> func,
                  ConnectionManager &userDefinedManagedConnections,
                  bool autoInvoke = true)
    {
        auto lockedSetting = this->data.lock();
        if (!lockedSetting) {
            return;
        }

        auto connection = lockedSetting->updated.connect(
            [=](const rapidjson::Value &, const SignalArgs &args) {
                func(args);  //
            });

        if (autoInvoke) {
            func(detail::onConnectArgs());
        }

        detail::connectToManager(userDefinedManagedConnections,
                                 std::move(connection));
    }

    // Static helper methods for one-offs (get or set setting)
    /// Create a temporary setting at the given path and read its value if it's already set
    ///
    /// Prefer to use the version where you pass your explicit SettingManager instance instead.
    static const Type
    get(const std::string &path, SettingOption options = SettingOption::Default)
    {
        Setting<Type> setting(path, options);

        return setting.getValue();
    }

    /// Create a temporary setting at the given path and read its value if it's already set
    static const Type
    get(const std::string &path,
        const std::shared_ptr<SettingManager> &instance,
        SettingOption options = SettingOption::Default)
    {
        Setting<Type> setting(path, options, instance);

        return setting.getValue();
    }

    /// Create a temporary setting at the given path and set its value on the static instance manager.
    ///
    /// Prefer to use the version where you pass your explicit SettingManager instance instead.
    static void
    set(const std::string &path, const Type &newValue,
        SettingOption options = SettingOption::Default)
    {
        Setting<Type> setting(path, options);

        setting.setValue(newValue);
    }

    /// Create a temporary setting at the given path and set its value on the static instance manager.
    static void
    set(const std::string &path, const Type &newValue,
        const std::shared_ptr<SettingManager> &instance,
        SettingOption options = SettingOption::Default)
    {
        Setting<Type> setting(path, options, instance);

        setting.setValue(newValue);
    }

private:
    std::vector<std::unique_ptr<Signals::ScopedConnection>> managedConnections;
};

}  // namespace pajlada::Settings
