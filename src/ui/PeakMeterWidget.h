#pragma once

#include <QWidget>
#include <QPainter>
#include <QSizePolicy>
#include <cmath>

class PeakMeterWidget : public QWidget {
public:
    explicit PeakMeterWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        setFixedHeight(22);
        setMinimumWidth(100);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override { return QSize(200, 22); }
    QSize minimumSizeHint() const override { return QSize(100, 22); }

    void setLevel(float level) {
        level_ = qBound(0.0f, level, 1.0f);
        update();
    }

    float level() const { return level_; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        int w = width();
        int meterH = 10;
        int meterY = 0;
        int tickY = meterH + 1;
        int tickH = 10;

        // Background
        p.fillRect(0, meterY, w, meterH, QColor(30, 30, 30));

        // Convert linear level to dB, then map to pixel position
        float levelDb = (level_ > 1e-6f)
            ? 20.0f * std::log10(level_)
            : minDb_;
        float barNorm = dbToNorm(levelDb);
        int barW = static_cast<int>(barNorm * w);

        if (barW > 0) {
            // Gradient on the dB scale
            QLinearGradient grad(0, 0, w, 0);
            float pos_m12 = dbToNorm(-12.0f);
            float pos_m3 = dbToNorm(-3.0f);
            grad.setColorAt(0.0, QColor(0, 180, 0));
            grad.setColorAt(pos_m12, QColor(0, 200, 0));
            grad.setColorAt(pos_m3, QColor(220, 220, 0));
            grad.setColorAt(1.0, QColor(220, 0, 0));
            p.fillRect(0, meterY, barW, meterH, grad);
        }

        // Border
        p.setPen(QColor(80, 80, 80));
        p.drawRect(0, meterY, w - 1, meterH - 1);

        // Tick marks on dB scale (evenly spaced in dB)
        QFont tickFont;
        tickFont.setPixelSize(9);
        p.setFont(tickFont);

        struct Tick { float db; const char* label; };
        static const Tick ticks[] = {
            {-48, "-48"}, {-36, "-36"}, {-24, "-24"},
            {-12, "-12"}, {-6, "-6"}, {-3, "-3"}, {0, "0"}
        };

        for (const auto& t : ticks) {
            float norm = dbToNorm(t.db);
            int x = static_cast<int>(norm * (w - 1));
            if (x < 1) continue;

            // Tick line on meter
            p.setPen(QColor(180, 180, 180));
            p.drawLine(x, meterY, x, meterH - 1);

            // Label below
            p.setPen(QColor(160, 160, 160));
            QRect textRect(x - 16, tickY, 32, tickH);
            p.drawText(textRect, Qt::AlignCenter, t.label);
        }
    }

private:
    float level_ = 0.0f;
    static constexpr float minDb_ = -60.0f;
    static constexpr float maxDb_ = 0.0f;

    // Map dB value to normalized 0..1 position on meter
    float dbToNorm(float db) const {
        db = qBound(minDb_, db, maxDb_);
        return (db - minDb_) / (maxDb_ - minDb_);
    }
};
