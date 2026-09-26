// InputHook: in-process system input interception for menu touch.
#pragma once

namespace InputHook {

void Start();
void SetEnabled(bool enabled);
bool IsActive();

} // namespace InputHook
