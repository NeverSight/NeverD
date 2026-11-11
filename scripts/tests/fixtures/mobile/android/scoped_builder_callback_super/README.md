# Scoped builder result and checked raw callback before super

An owned generic child retains the original virtual builder result and a fresh
raw Function/Serializable object, including both explicit check-casts, before
the selected Map/Function parent constructor. A frozen future builder returns
an unrelated opaque map and mutates its original field. Four constructor routes
compare 96 cases twice for source and SDK builders/parents: allocation and call
order, exact receivers, input/result snapshots, callback identity and argument,
partial writes, exact failures and later key casts. Every recovered unit and
original metadata/bridge entry is compiled and audited, including a methodless
marker. Wrong programs and Code-preserving Signature controls guard the observer
and scope. Only owned builders, callbacks and maps execute.
