# Bounded seed with primitive auxiliaries and a fixed result

An original javac private static method has one plain nominally bounded formal,
a direct seed of that formal, two float parameters and a fixed nongeneric result
interface. Complete original Signatures and exact private declaring-class scope
must be retained. Its ordinary bytecode body remains responsible for null
branches, ordered effects, partial trace state, failures and the actual result.

Frozen calls, reflection and method handles compare original and recovered code
for distinct NaN payloads, signed zero, infinities, float extrema, null inputs
and outputs, nominal subclasses and all effect failure sites. Four independent
wrong programs must be detected. The bound and result declarations are also
retested as an unchanged dependency input.
