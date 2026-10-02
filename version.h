/* Single source of truth for the project version.
 *
 * Included both by resource.rc (windres runs the C preprocessor) and by the C++
 * code, so the version compiled into the executable's resources, the version a
 * front end prints, and the git tag can never disagree.
 */
#ifndef OPENCODE_SM_VERSION_H
#define OPENCODE_SM_VERSION_H

#define OPENCODE_SM_VERSION_MAJOR 0
#define OPENCODE_SM_VERSION_MINOR 1
#define OPENCODE_SM_VERSION_PATCH 3
#define OPENCODE_SM_VERSION_STRING "0.1.3"

#endif /* OPENCODE_SM_VERSION_H */
