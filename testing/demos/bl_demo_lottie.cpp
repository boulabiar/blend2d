#include "bl_qt_headers.h"
#include "bl_qt_canvas.h"
#include "bl_lottie.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtGui/QKeySequence>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QShortcut>
#include <algorithm>

class MainWindow : public QWidget {
  Q_OBJECT

public:
  explicit MainWindow(const QStringList& animation_paths, QWidget* parent = nullptr)
    : QWidget(parent),
      _animation_paths(animation_paths) {
    auto* layout = new QVBoxLayout();
    auto* controls = new QHBoxLayout();

    controls->addWidget(new QLabel(QStringLiteral("Renderer:")));
    QBLCanvas::init_renderer_select_box(&_renderer_select, true);
    controls->addWidget(&_renderer_select);
    connect(&_renderer_select, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onRendererChanged);

    controls->addWidget(new QLabel(QStringLiteral("Animation:")));
    for (const QString& path : _animation_paths)
      _animation_select.addItem(QFileInfo(path).fileName(), path);
    controls->addWidget(&_animation_select);
    connect(&_animation_select, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onAnimationChanged);

    _play_button.setText(QStringLiteral("Pause"));
    controls->addWidget(&_play_button);
    connect(&_play_button, &QPushButton::clicked, this, &MainWindow::onTogglePlay);

    _restart_button.setText(QStringLiteral("Restart"));
    controls->addWidget(&_restart_button);
    connect(&_restart_button, &QPushButton::clicked, this, &MainWindow::onRestart);

    controls->addStretch();
    layout->addLayout(controls);
    layout->addWidget(&_canvas, 1);
    setLayout(layout);

    _canvas.on_render_blend2d = [this](BLContext& ctx) noexcept { onRenderBlend2D(ctx); };

    connect(&_timer, &QTimer::timeout, this, &MainWindow::onTimer);
    _timer.setInterval(16);

    if (!_animation_paths.isEmpty()) {
      loadAnimation(_animation_paths.first());
      _animation_select.setCurrentIndex(0);
    }

    _elapsed.start();
    _last_time_ms = _elapsed.elapsed();
    _timer.start();

    auto* toggleShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(toggleShortcut, &QShortcut::activated, this, &MainWindow::onTogglePlay);
  }

private Q_SLOTS:
  void onRendererChanged(int index) {
    _canvas.set_renderer_type(_renderer_select.itemData(index).toInt());
  }

  void onAnimationChanged(int index) {
    if (index < 0 || index >= _animation_paths.size())
      return;
    loadAnimation(_animation_paths.at(index));
  }

  void onTogglePlay() {
    _playing = !_playing;
    _play_button.setText(_playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
  }

  void onRestart() {
    if (!_composition.is_valid())
      return;
    _current_frame = _composition.in_point();
    _elapsed.restart();
    _last_time_ms = 0;
    _canvas.update_canvas(true);
    updateTitle();
  }

  void onTimer() {
    if (!_composition.is_valid())
      return;

    qint64 now = _elapsed.elapsed();
    double dt = (now - _last_time_ms) / 1000.0;
    _last_time_ms = now;
    if (dt < 0.0 || dt > 1.0)
      dt = 0.0;

    if (_playing) {
      double rate = _composition.frame_rate();
      double start = _composition.in_point();
      double end = _composition.out_point();
      double span = end - start;
      if (span <= 0.0)
        span = rate > 0.0 ? rate : 1.0;

      _current_frame += dt * rate;
      while (_current_frame >= end)
        _current_frame -= span;
      while (_current_frame < start)
        _current_frame += span;
    }

    _canvas.update_canvas(true);
    updateTitle();
  }

private:
  void loadAnimation(const QString& path) {
    QString error;
    if (!_composition.load_from_file(path, &error)) {
      QMessageBox::warning(this, QStringLiteral("Lottie Load Failed"), error);
      return;
    }

    _current_path = path;
    _current_frame = _composition.in_point();
    _elapsed.restart();
    _last_time_ms = 0;
    _canvas.update_canvas(true);
    updateTitle();
  }

  void onRenderBlend2D(BLContext& ctx) noexcept {
    ctx.clear_all();
    ctx.fill_all(BLRgba32(0xFF1E1E1E));

    if (!_composition.is_valid())
      return;

    double comp_w = _composition.width();
    double comp_h = _composition.height();
    if (comp_w <= 0.0 || comp_h <= 0.0)
      return;

    double canvas_w = double(_canvas.image_width());
    double canvas_h = double(_canvas.image_height());
    if (canvas_w <= 0.0 || canvas_h <= 0.0)
      return;

    double scale = std::min(canvas_w / comp_w, canvas_h / comp_h);
    if (scale <= 0.0)
      scale = 1.0;

    double offset_x = (canvas_w - comp_w * scale) * 0.5;
    double offset_y = (canvas_h - comp_h * scale) * 0.5;

    BLMatrix2D translation = BLMatrix2D::make_translation(offset_x, offset_y);
    BLMatrix2D scaling = BLMatrix2D::make_scaling(scale, scale);
    BLMatrix2D root = lottie_matrix_multiply(translation, scaling);

    _composition.render(ctx, _current_frame, root, 1.0);
  }

  void updateTitle() {
    QString name = _current_path.isEmpty() ? QStringLiteral("None") : QFileInfo(_current_path).fileName();
    QString title = QStringLiteral("Lottie Demo | %1 | Frame %.1f | Render %.2f ms | FPS %.1f")
      .arg(name)
      .arg(_current_frame, 0, 'f', 1)
      .arg(_canvas.average_render_time())
      .arg(_canvas.fps());
    if (title != windowTitle())
      setWindowTitle(title);
  }

  QStringList _animation_paths;
  QString _current_path;
  LottieComposition _composition;
  QBLCanvas _canvas;
  QComboBox _renderer_select;
  QComboBox _animation_select;
  QPushButton _play_button;
  QPushButton _restart_button;
  QTimer _timer;
  QElapsedTimer _elapsed;
  qint64 _last_time_ms {};
  bool _playing {true};
  double _current_frame {};
};

static QStringList gatherSearchRoots() {
  QStringList roots;
  QDir dir(QCoreApplication::applicationDirPath());
  roots << dir.absolutePath();
  for (int i = 0; i < 5; i++) {
    dir.cdUp();
    roots << dir.absolutePath();
  }
  roots.removeDuplicates();
  return roots;
}

static QString locateAnimation(const QString& input, const QStringList& roots) {
  QFileInfo info(input);
  if (info.isAbsolute() && info.exists())
    return info.absoluteFilePath();

  if (info.exists())
    return info.absoluteFilePath();

  for (const QString& root : roots) {
    QDir dir(root);
    QString direct = dir.absoluteFilePath(input);
    if (QFileInfo::exists(direct))
      return direct;

    QString in_lottie = dir.absoluteFilePath(QStringLiteral("lottie/%1").arg(input));
    if (QFileInfo::exists(in_lottie))
      return in_lottie;
  }
  return QString();
}

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationDisplayName(QStringLiteral("Blend2D Lottie Demo"));

  QStringList roots = gatherSearchRoots();
  QStringList args = app.arguments();
  QStringList animations;

  for (int i = 1; i < args.size(); i++) {
    QString located = locateAnimation(args.at(i), roots);
    if (!located.isEmpty())
      animations << located;
  }

  if (animations.isEmpty()) {
    QStringList defaults {QStringLiteral("testTiger.json"), QStringLiteral("Shopping Bag.json")};
    for (const QString& name : defaults) {
      QString located = locateAnimation(name, roots);
      if (!located.isEmpty())
        animations << located;
    }
  }

  animations.removeDuplicates();

  if (animations.isEmpty()) {
    QMessageBox::critical(nullptr, QStringLiteral("Lottie Demo"), QStringLiteral("No Lottie animations were found."));
    return 1;
  }

  MainWindow window(animations);
  window.setMinimumSize(QSize(480, 360));
  window.resize(QSize(720, 720));
  window.show();

  return QApplication::instance()->exec();
}

#include "bl_demo_lottie.moc"
