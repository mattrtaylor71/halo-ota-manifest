# Provisioning completion and foreground sleep recovery

This change addresses the observed success-screen blackout on the bench's
LCD201 with private Sense205. It does not change Wi-Fi, account claims, image
uploads, voice, OTA policy or the production 02:00 Pacific schedule. Public
paired OTA remains201 and EOL remains197 until a separately recorded release.

## Captured failure

Evidence is under
`/Users/MattTaylor/halo-provision-memory202-20260921/reprovision-camera205-01`.
The passive capture sent no commands and was closed and reaped after the case.
`CASE-RESULT.json` binds both timestamped logs and the capture event log.

The first setup exhausted four connection attempts without an HTTP response.
After the user retried, the third claim attempt returned HTTP200 in1092ms and
the owner was persisted at epoch1790058599.102576. Account linking succeeded.
No image capture followed this setup, so205's same-wake image pacing remains
unqualified by this case.

The LCD's five-minute guardian had already expired during the prolonged setup
at epoch1790058528.06. It rendered Complete at8599.550201. A repeated connected
heartbeat at8602.786780 cleared the legacy peer provisioning flag. Complete
had also cleared the UI-owned progress sleep lease, so the overdue guardian
entered the sleep handshake before the intended4.5-second success display ended.
Sense denied sleep because its15-second successful-setup grace was still active.
LCD consequently darkened the panel and waited for Sense.

A tap at8605.314311 restored the display, but it darkened again at8605.444612.
Fresh taps cleared the passive wait while leaving the guardian expired; each
therefore started another denied sleep attempt. The denial count reached10.
A late touch then aborted forced teardown. The boards eventually slept normally.
There was no observed reboot or lost account ownership in this interval.

## Correction

- The existing atomic provisioning lease now covers successful completion as
  well as Connecting. Complete gets4.5seconds plus a finite10-second handoff
  margin. Repeated status messages do not renew it; a stopped UI still expires.
- The UI owner starts a fresh guardian interval before releasing that lease
  and handing control to Home. Normal ten-second menu inactivity remains.
- Actual touch/scroll wake and real-input teardown recovery start a fresh
  five-minute guardian interval. Generic activity resets, firmware-info retries
  and incoming UART messages do not.
- Cancelling an old sleep episode for user input clears that episode's denial
  count, preventing a later tap from inheriting forced teardown.
- Guardian timestamps are atomic. Age calculation treats a concurrently newer
  start as fresh activity and handles32-bit wrap without unsigned underflow.

The regular sleep loop and provisioning UI service already share the LVGL
lock, serializing completion-to-Home against guardian admission. Existing
handshake touch checks and the teardown ISR remain in place. Media custody,
finite OTA holds, denial backoff and passive peer waits are retained.

## Qualification status at source commit

Regression tests execute the actual flow, guardian, sleep funnel and user-wake
functions with controlled hardware boundaries. They cover the recorded long
setup sequence, repeated connected heartbeats, completion expiry, fresh touches,
denial retirement, unattended guardian recovery, wraparound and unchanged
background visibility. Build and physical results will be appended after they
exist; source tests are not a device acceptance pass.

The separate Sense post-AP claim retry defect discovered during the first setup
is still pending: exhausted incomplete attempts are not reset, and inactive
setup returns before the retry can submit. Do not describe that recovery as
fixed by this LCD change.
