import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

part of dart.ui;

const pragma entryPoint = pragma('vm:entry-point', 'call');
const pragma neverInline = pragma('vm:never-inline');

typedef MallocC = Pointer Function(IntPtr size);
typedef MallocDart = Pointer Function(int size);
typedef FreeC = Void Function(Pointer ptr);
typedef FreeDart = void Function(Pointer ptr);

final DynamicLibrary _libc = Platform.isWindows
    ? DynamicLibrary.open('msvcrt.dll') // Windows 的 C 标准库
    : DynamicLibrary.process(); // Unix 系统中 libc 默认加载

// 获取 malloc 方法
final MallocDart _posixMalloc =
_libc.lookupFunction<MallocC, MallocDart>('malloc');
final FreeDart _posixFree = _libc.lookupFunction<FreeC, FreeDart>('free');

Pointer<T> _allocate<T extends NativeType>(int byteCount, {int? alignment}) {
  Pointer<T> result;
  result = _posixMalloc(byteCount).cast();
  if (result.address == 0) {
    throw ArgumentError('Could not allocate $byteCount bytes.');
  }
  return result;
}

void _free(Pointer pointer) {
  _posixFree(pointer);
}

class MMTrace {
  static Pointer<Uint8> _paramsPointer = nullptr;
  static int _pointerArrayLength = 0;
  static bool enable = false;

  static void trace(Function func, bool unwind, List<int> params) {
    if (!enable) {
      return;
    }
    bool debugMode = false;
    assert(() {
      debugMode = true;
      return true;
    }());
    if (debugMode) {
      return;
    }
    if (Platform.isWindows) {
      return;
    }
    Pointer<Uint8> paramsPointer = nullptr;
    if (params.isNotEmpty) {
      if (_paramsPointer != nullptr &&
          _pointerArrayLength <= 4 * params.length) {
        paramsPointer = _paramsPointer;
      } else {
        if (_paramsPointer != nullptr) {
          _free(_paramsPointer);
        }
        paramsPointer = _allocate<Uint8>(4 * params.length);
        _paramsPointer = paramsPointer;
        _pointerArrayLength = 4 * params.length;
      }
      final dataList =
      paramsPointer
          .asTypedList(4 * params.length)
          .buffer
          .asByteData();
      for (int i = 0; i < params.length; i++) {
        dataList.setInt32(i * 4, params[i], Endian.host);
      }
    }
    _trace(func, unwind, paramsPointer, params.length);
  }

  @Native<Void Function(Handle, Bool, Pointer<Uint8>, Int32)>(
      symbol: 'mm_trace_tools::DartTrace')
  external static void _trace(Function func, bool unwind, Pointer<Uint8> params,
      int paramsNum);
}

enum _FluencyTrackerStage {
  build,
  layout,
  compositing,
  paint,
  compositeFrame,
  semantics,
}

enum _FluencyTrackerFrameRequestType {
  setState,
  tick,
}

typedef ItemTrans = Object? Function(Object obj);

class MMFluencyTracker {

  @pragma('vm:entry-point')
  static void _appFunc() {}

  MMFluencyTracker._(this._applicationId) {
    if (Platform.isIOS) { // only used for ios
      _nativeBindAppFunc(_applicationId, _appFunc);
    }
  }

  static ItemTrans? itemTrans;
  // 只有 enable 为 true 时，enableDartTrace 和 enableFrameInfoTrace 才可以为 true
  static bool enable = false;
  // enableDartTrace 为 true 时，enableFrameInfoTrace 必须为 true
  static bool enableDartTrace = false;
  // 仅记录 build/layout 等耗时
  static bool enableFrameInfoTrace = false;
  static int maxRecordItemSize = 10;

  final int _applicationId;

  final Set<Object> _affectedItem = Set();

  static MMFluencyTracker get instance =>
      Application.current.get(
          #MMFluencyTracker, () => MMFluencyTracker._(Application.current.id));

  int _frameNumber = 0;

  void setTraceConfig(String traceConfig, String router) {
    if (!enable) {
      return;
    }
    _nativeSetTraceConfig(_applicationId, traceConfig, router);
  }

  void triggerSummaryReport() {
    if (!enable) {
      return;
    }
    _nativeTriggerSummaryReport(_applicationId);
  }

  void reportGfxInfo() {
    if (!enable) {
      return;
    }
    _nativeReportGfxInfo();
  }

  void bindFrameNumber(int frameNumber) {
    if (frameNumber < 0) {
      return;
    }
    _frameNumber = frameNumber;
  }

  void build(bool start) {
    _fluencyStageChange(_FluencyTrackerStage.build, start);
  }

  void layout(bool start) {
    _fluencyStageChange(_FluencyTrackerStage.layout, start);
  }

  void compositing(bool start) {
    _fluencyStageChange(_FluencyTrackerStage.compositing, start);
  }

  void paint(bool start) {
    _fluencyStageChange(_FluencyTrackerStage.paint, start);
  }

  void compositeFrame(bool start) {
    _fluencyStageChange(_FluencyTrackerStage.compositeFrame, start);
  }


  void semantics(bool start) {
    _fluencyStageChange(_FluencyTrackerStage.semantics, start);
  }

  void setState(Object? affected) {
    _markFrameRequest(_FluencyTrackerFrameRequestType.setState, true, affected);
  }

  void onTick(Object? affected) {
    _markFrameRequest(_FluencyTrackerFrameRequestType.tick, false, affected);
  }

  void appendDirtyItem(Object obj) {
    if (!enable || !enableDartTrace) {
      return;
    }
    if (_affectedItem.length < maxRecordItemSize) {
      final result = itemTrans?.call(obj);
      if (result != null) {
        _affectedItem.add(result);
      }
    }
  }

  void appendDirtyItems(List<Object> objs) {
    if (!enable || !enableDartTrace) {
      return;
    }
    if (itemTrans != null) {
      _affectedItem.addAll(
          objs
              .take(maxRecordItemSize)
              .map(itemTrans!)
              .whereType<Object>()
              .toList());
    }
  }

  @pragma('vm:prefer-inline')
  void _fluencyStageChange(_FluencyTrackerStage stage, bool start) {
    if ((!enable || !enableDartTrace) && !enableFrameInfoTrace) {
      return;
    }
    if (start) {
      _affectedItem.clear();
    }
    _nativeFluencyStageChange(
        _applicationId, _frameNumber, stage.index, start,
        _affectedItem.toList(growable: false));
  }

  @pragma('vm:prefer-inline')
  void _markFrameRequest(_FluencyTrackerFrameRequestType requestType,
      bool needStackTrace, Object? type) {
    if (!enable || !enableDartTrace) {
      return;
    }
    _nativeMarkFrameRequest(
        _applicationId, requestType.index, needStackTrace, type);
  }

  @Native<Void Function(Uint64, Uint64, Uint64, Bool, Handle)>(
      symbol: 'flutter_fluency_tracker::DartFluencyStageChange')
  external static void _nativeFluencyStageChange(int applicationId,
      int frameNumber, int newStage, bool start, Object types);

  @Native<Void Function(Uint64, Uint64, Bool, Handle)>(
      symbol: 'flutter_fluency_tracker::DartMarkFrameRequest')
  external static void _nativeMarkFrameRequest(int applicationId,
      int reqType, bool needStackTrace, Object? type);

  @Native<Void Function(Uint64, Handle, Handle)>(
      symbol: 'flutter_fluency_tracker::DartSetTraceConfig')
  external static void _nativeSetTraceConfig(int applicationId, String traceConfig,
      String router);

  @Native<Void Function(Uint64)>(
      symbol: 'flutter_fluency_tracker::DartTriggerSummaryReport')
  external static void _nativeTriggerSummaryReport(int applicationId);

  @Native<Void Function(Uint64, Handle)>(
      symbol: 'flutter_fluency_tracker::DartBindAppFunc')
  external static void _nativeBindAppFunc(int applicationId, Function func);

  @Native<Void Function()>(
      symbol: 'flutter_fluency_tracker::ReportGfxInfo')
  external static void _nativeReportGfxInfo();

}
