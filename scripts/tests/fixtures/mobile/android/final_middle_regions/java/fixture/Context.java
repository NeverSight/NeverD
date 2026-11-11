package fixture;
public interface Context {
 Object open(Core target);
 Object left(Core target);
 Object right(Core target);
 void after(Core target,int branch);
 Object step(Core target,int index);
 Object other(Core target,int index);
 RuntimeException problem(Core target);
 Object finish(Core target,Object current);
}
