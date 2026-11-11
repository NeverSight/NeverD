# Unchanged constructor arguments

An owned copy constructor computes a branched superclass input while forwarding an unchanged child object through a superclass parameter. Its continuation still reads a child field from the original argument. Source and SDK profiles preserve argument identity, partial construction, mutation during the parent constructor, exact failures, and original compiler metadata. Private inputs are excluded.
