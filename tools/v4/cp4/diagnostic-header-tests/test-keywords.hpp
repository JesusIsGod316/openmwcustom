#ifndef OPENMW_DIAGNOSTIC_TEST_KEYWORDS_H
#define OPENMW_DIAGNOSTIC_TEST_KEYWORDS_H
#if defined(OPENMW_TEST_REAL_QT)
#include <QtCore/QObject>
#else
// Same empty keyword expansion that caused the Qt consumer failure. Native Qt
// variants are separate, explicit tests; this portable fixture is not Qt itself.
#define emit
#define slots
#define signals public
#endif
#endif
