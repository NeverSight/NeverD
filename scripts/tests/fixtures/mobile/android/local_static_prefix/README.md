# Lexically owned static reads in constructor prefixes

Unmodified Kotlin class-lambda output retains two anonymous callback singleton
classes enclosed by a synthetic default constructor. The constructor reads both
public static singleton fields before delegating to the primary constructor.
The original lexical visibility proof and shared source member binding compose
without making other method scopes or private fields accessible.

Source and SDK-callback profiles compile every generated Java unit and compare
all definitions and metadata/Exceptions against individually reported anonymous
bindings, Signature projections and synthetic-flag loss. Frozen callers exercise
every mask combination, partial initialization, callback identity across distinct
objects, null values, return identity and runtime/error failure timing.
