package fixture; public class Parent { public final int seed; public final Token token; public Parent(int seed,Token token) { this.seed=seed; this.token=token; Trace.parent(this,seed,token); } }
