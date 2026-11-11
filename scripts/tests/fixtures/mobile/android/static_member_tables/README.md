# Static-only member tables

An owned classfile witness removes only an unused private empty member constructor and its static InnerClasses bit. All retained method Code, fields and other metadata stay frozen. The resulting non-static member has only static state and methods. Source and dependency profiles compare lazy initialization, cached table identity, mutation, discarded results, primitive reads, null/bounds failures and class-initialization exception order. Additional compiler constructor/outer-field roles and any source metadata differences must be explicit in the recovery report.
