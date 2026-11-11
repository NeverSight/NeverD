package fixture;
public final class Core {
 public final Object first,selected,last;
 public Core(Context context,int choice,int rounds,int kind) {
  first=context.open(this);
  if(choice==0) {
   selected=context.left(this);
   context.after(this,0);
  } else {
   selected=context.right(this);
   context.after(this,1);
  }
  Object current=null;
  for(int i=0;i<rounds;i++) {
   switch(kind) {
    case 0:current=context.step(this,i);break;
    case 1:current=context.other(this,i);break;
    default:throw context.problem(this);
   }
  }
  last=context.finish(this,current);
 }
}
