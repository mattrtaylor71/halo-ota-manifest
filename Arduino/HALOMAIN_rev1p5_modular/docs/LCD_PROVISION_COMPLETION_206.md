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

## Build and bench results

Source `b0667160c4bc26f4383b50f56094e26dc96b4011`, firmware tree
`ff31282afe47d7c67554ea9885861b2dee0335ca`, produced private LCD build
`6.4.206-20260922T064705Z-b0667160c4bc`. The203 camera-aware artifact checker
passed the unchanged canonical LCD production profile. No Sense206 was built
and no OTA was published.

LCD BIN:2033104bytes, SHA256
`19df338b6065f0daed9e521f508a8d9ee7deba1333ecfd57e3052a79c27ed344`.
Both working-tree and exact-snapshot full gates passed all112 suites. The
focused39-case flow/funnel suite reproduced seven expected failures against
the retained205 source; three comparison controls remained passing.

`service-lcd206-01` installed206/app0, retaining201/app1, NVS, partition table,
filesystem and the old selected VALID selector. Both full application banks
were backed up, candidate and alternate NEW selector were read back, and all
service/capture owners closed and reaped. This is scoped USB service, not OTA.

`postservice206-health02/RESULT.json` passed a fresh actuator wake, nonce/CRC
checked LCD206/app0/bootapp0 SDKVALID identity, fresh Sense205/app0/bootapp0
SDKVALID reply, unlocked Home, normal coordinated sleep and five seconds
without USB reopening. All29 LCD heartbeats were lit before normal sleep.
The next timers remained02:00Pacific for Sense and01:59:45 for LCD.

The earlier health01 observation is retained as a host-check failure: it saved
ID1 during the startup OTA lease, then checked that stale busy reply after
Home unlocked. Correct firmware/build/SDKVALID were already observed. Health02
waited for startup work to settle and required a fresh idle reply; no firmware
or persistent state was changed to pass it.

## User provisioning and Check-in retest

`reprovision-lcd206-01/CASE-RESULT.json` records the user's fresh phone setup
and physical Check-in/Confirm. Claim succeeded on its second attempt. Complete
rendered at epoch1790060922.351491 and returned Home at0926.8454, about4.5s
later; a repeated connected heartbeat did not darken it. Check-in and quantity1
confirmation were acknowledged. Home returned at0932.3716 and the first dark
event was0942.383601, the normal10.012-second idle interval. All40 observed
heartbeats before that dark event showed the panel lit. No guardian event or
premature sleep was observed. There were no heartbeat samples after idle-dark;
do not infer continuous sampled backlight proof during the remaining upload.

The154506-byte image failed its first PUT with an AES512-byte allocation
failure, then uploaded on the immediate second attempt. Read-only cloud
checksum and full JPEG decode passed. Both boards slept normally; capture
owners closed and reaped with no device commands sent. The next nightly arm
remained02:00Pacific. The [Sense205 record](IMAGE_UPLOAD_PACING_205.md) retains
the unresolved first-attempt memory issue and the delivery test's limits.

This confirms the normal physical setup/completion/menu/capture flow on206.
The case used a fresh wake and completed setup before five minutes, so actual
expired-guardian physical reproduction remains unqualified. The exact prolonged
sequence has host regression and retained pre-fix negative-control evidence.
