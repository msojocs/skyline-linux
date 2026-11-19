// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

part of dart.ui;

// Skyline compatibility layer.
//
// The private Flutter engine shipped inside `skyline.node` adds a per-application
// `dart:ui` layer (`dart:ui/application.dart`).  The source below is recovered
// from the official `isolate_snapshot_data` and adapted to the public engine:
//
//   * the public engine has one `PlatformDispatcher` singleton, so `Application`
//     exposes that singleton instead of allocating a per-application dispatcher;
//   * the official `Application.top` recurses into `current` when the stack is
//     empty, which assumes the embedder always runs Dart inside a zone carrying
//     `kApplicationId`.  Here `top` lazily creates the default application so the
//     class also works from the root zone of the public engine.
//
// Keep the public member surface in sync with `spec/kernel-symbols.json`
// (`dartUiCustomMembers`) and `docs/engine-symbols.md`.

/// Key of the flutter application id.
const Symbol kApplicationId = #flutter.io.application_id;

/// The default id of the flutter application.
const int kDefaultApplicationId = 0;

Map<Object, Application> _applications = <Object, Application>{};

Set<Object> _exitedApplicationIds = <Object>{};

/// Stack of application ids, used by [Application.top].
List<int> stack = <int>[];

/// The 'Application' represents a flutter application; each flutter application
/// has its own 'ui.window', 'ui.PlatformDispatcher' and so on.  Each application
/// has its own element tree, render object tree, and layer tree.  On the engine
/// side, each application has a corresponding 'flutter::Shell'.  There can be
/// multiple flutter applications in the root isolate.
class Application {
  /// Get the current 'Application' according to the application id in the zone.
  static Application get current {
    final int? applicationId = Zone.current[kApplicationId] as int?;
    if (applicationId == null) {
      return top;
    }
    return Application.fromId(applicationId);
  }

  static Application get top {
    if (stack.isEmpty) {
      return fromId(kDefaultApplicationId);
    }
    return fromId(stack.last);
  }

  /// Get the 'Application' according to the specified application id.
  static Application fromId(int applicationId) {
    assert(!_exitedApplicationIds.contains(applicationId));
    final Application result =
        _applications[applicationId] ??= Application._(applicationId);
    return result;
  }

  static bool get isAlive {
    final int? applicationId = Zone.current[kApplicationId] as int?;
    if (applicationId == null) {
      return true;
    }
    return !_exitedApplicationIds.contains(applicationId);
  }

  Application._(int id)
      : _id = id,
        _zone =
            Zone.current.fork(zoneValues: <Object, Object>{kApplicationId: id}),
        _platformDispatcher = PlatformDispatcher.instance,
        _channelBuffers = ChannelBuffers() {
    _window = SingletonFlutterWindow._();
    stack.add(id);
  }

  /// The id of the flutter application.
  int get id => _id;
  final int _id;

  /// The zone the logic of this 'Application' should run in.
  Zone get zone => _zone;
  final Zone _zone;

  PlatformDispatcher get platformDispatcher => _platformDispatcher;
  final PlatformDispatcher _platformDispatcher;

  SingletonFlutterWindow get window => _window;
  late SingletonFlutterWindow _window;

  ChannelBuffers get channelBuffers => _channelBuffers;
  final ChannelBuffers _channelBuffers;

  final Map<Object, Object?> _values = <Object, Object?>{};

  /// Whether this is the default flutter application.
  bool isDefaultApplication() => _id == kDefaultApplicationId;

  /// Put the object for the key.
  void put(Object key, Object? value) => _values[key] = value;

  /// Find the object for the key, returning null if not found.
  T? find<T>(Object key) => _values[key] as T?;

  /// Get the object for the key, creating it with [ifAbsent] if necessary.
  T get<T extends Object>(Object key, T Function() ifAbsent) {
    T? result = find(key);
    if (result == null) {
      result = ifAbsent();
      put(key, result);
    }
    return result;
  }

  /// Called when the application exits.
  bool _exit() {
    assert(!_exitedApplicationIds.contains(_id));
    stack.remove(_id);
    _values.clear();
    _applications.remove(_id);
    _exitedApplicationIds.add(_id);
    return true;
  }
}
