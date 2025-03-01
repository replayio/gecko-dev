/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://w3c.github.io/hr-time/#sec-performance
 * https://w3c.github.io/navigation-timing/#extensions-to-the-performance-interface
 * https://w3c.github.io/performance-timeline/#extensions-to-the-performance-interface
 * https://w3c.github.io/resource-timing/#sec-extensions-performance-interface
 * https://w3c.github.io/user-timing/#extensions-performance-interface
 *
 * Copyright © 2015 W3C® (MIT, ERCIM, Keio, Beihang).
 * W3C liability, trademark and document use rules apply.
 */

typedef double DOMHighResTimeStamp;
typedef sequence <PerformanceEntry> PerformanceEntryList;

// https://w3c.github.io/hr-time/#sec-performance
[Exposed=(Window,Worker)]
interface Performance : EventTarget {
  // Disallow code transformations related to performance.now() calls. This is needed
  // to ensure these calls occur at consistent points when recording/replaying, and
  // it also ensures that developers using performance.now() will be able to measure
  // the timing for the things they think they are measuring.
  //
  // See https://github.com/RecordReplay/backend/issues/4405
  //[DependsOn=DeviceState, Affects=Nothing]
  DOMHighResTimeStamp now();

  [Constant]
  readonly attribute DOMHighResTimeStamp timeOrigin;

  [Default] object toJSON();
};

// https://w3c.github.io/navigation-timing/#extensions-to-the-performance-interface
[Exposed=Window]
partial interface Performance {
  [Constant]
  readonly attribute PerformanceTiming timing;
  [Constant]
  readonly attribute PerformanceNavigation navigation;
};

// https://w3c.github.io/performance-timeline/#extensions-to-the-performance-interface
[Exposed=(Window,Worker)]
partial interface Performance {
  PerformanceEntryList getEntries();
  PerformanceEntryList getEntriesByType(DOMString entryType);
  PerformanceEntryList getEntriesByName(DOMString name, optional DOMString
    entryType);
};

// https://w3c.github.io/resource-timing/#sec-extensions-performance-interface
[Exposed=(Window,Worker)]
partial interface Performance {
  void clearResourceTimings();
  void setResourceTimingBufferSize(unsigned long maxSize);
  attribute EventHandler onresourcetimingbufferfull;
};

// GC microbenchmarks, pref-guarded, not for general use (bug 1125412)
[Exposed=Window]
partial interface Performance {
  [Pref="dom.enable_memory_stats"]
  readonly attribute object mozMemory;
};

// https://w3c.github.io/user-timing/#extensions-performance-interface
[Exposed=(Window,Worker)]
partial interface Performance {
  [Throws]
  void mark(DOMString markName);
  void clearMarks(optional DOMString markName);
  [Throws]
  void measure(DOMString measureName, optional DOMString startMark, optional DOMString endMark);
  void clearMeasures(optional DOMString measureName);
};

[Exposed=Window]
partial interface Performance {
  [Pref="dom.enable_event_timing", SameObject]
  readonly attribute EventCounts eventCounts;
};

