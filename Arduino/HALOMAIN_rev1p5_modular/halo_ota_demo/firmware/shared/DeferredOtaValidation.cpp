// Strong C hook overrides Arduino's weak pre-setup validation default.
// No allocation or SDK work here: the application's local health gate owns
// PENDING_VERIFY -> VALID or rollback after setup/loop readiness.
extern "C" bool verifyRollbackLater(void) { return true; }
