#include <aegis/engine/engine.hpp>

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::uint16_t port{8583};
    std::filesystem::path genesis;
    bool genesis_set{false};
};

void show_usage() {
    QMessageBox::information(
        nullptr,
        "aegis-console",
        "usage: aegis-console --genesis <path> [--port <0-65535>]");
}

[[nodiscard]] bool parse_port(std::string_view text, std::uint16_t& out) {
    try {
        const unsigned long value = std::stoul(std::string{text});
        if (value > 65535UL) {
            return false;
        }
        out = static_cast<std::uint16_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto next = [&](std::string_view flag) -> std::string_view {
            if (i + 1 >= argc) {
                QMessageBox::critical(nullptr, "aegis-console",
                                      QString("missing value for %1").arg(QString::fromUtf8(flag.data(),
                                                                                            static_cast<int>(flag.size()))));
                std::exit(2);
            }
            return std::string_view{argv[++i]};
        };

        if (arg == "--help" || arg == "-h") {
            show_usage();
            std::exit(0);
        }
        if (arg == "--port") {
            if (!parse_port(next(arg), options.port)) {
                QMessageBox::critical(nullptr, "aegis-console", "invalid port");
                return false;
            }
            continue;
        }
        if (arg == "--genesis") {
            options.genesis = std::filesystem::path{std::string{next(arg)}};
            options.genesis_set = true;
            continue;
        }

        QMessageBox::critical(nullptr, "aegis-console",
                              QString("unknown argument '%1'").arg(QString::fromUtf8(arg.data(), static_cast<int>(arg.size()))));
        return false;
    }

    if (!options.genesis_set) {
        QMessageBox::critical(nullptr, "aegis-console", "missing required --genesis <path>");
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    Options options;
    if (!parse_options(argc, argv, options)) {
        show_usage();
        return 2;
    }

    aegis::engine::EngineConfig config{};
    config.genesis_path = options.genesis;
    config.listen_port = options.port;

    aegis::engine::Engine engine;
    if (!engine.start(config).has_value()) {
        QMessageBox::critical(
            nullptr,
            "aegis-console",
            QString("engine failed to start with genesis '%1'")
                .arg(QString::fromStdString(options.genesis.string())));
        return 1;
    }

    QWidget window;
    window.setWindowTitle(QString("127.0.0.1:%1").arg(engine.port()));
    window.resize(320, 120);

    auto* layout = new QVBoxLayout(&window);
    auto* label = new QLabel(QString::number(engine.auth_counter().snapshot_count()), &window);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&engine, label]() {
        label->setText(QString::number(engine.auth_counter().snapshot_count()));
    });
    timer.start(33);

    window.show();

    const int exit_code = app.exec();
    engine.stop();
    return exit_code;
}
