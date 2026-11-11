The instance-marker profile retains an instance void call between allocation and
constructor invocation. Its receiver is read from the original static field;
class initialization, exception identity, cached initialization failures, call
counts and partially completed constructors are observed through four frozen
call routes. Both source and SDK-marker profiles compile every emitted Java unit
and compare the complete supported metadata and Exceptions. Five independent
wrong programs cover deleted/duplicated calls, null receivers, extra operand
checks and moving the call before allocation. The original static and discarded
result profiles retain their existing inputs and behavior.
