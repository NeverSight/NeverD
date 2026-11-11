import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedNominalArrayConstructorHarness {
  private static final class Child extends Item {}
  static void require(boolean ok){if(!ok)throw new AssertionError("owned closed nominal array constructor mismatch");}
  static Core call(int route,int operation,Item[][] items,Context context,String[] labels)throws Throwable {
    if(route==0)return operation==0?new Core(items,context,labels):new Core(items,labels);
    if(route==1)return operation==0?ClosedNominalArrayConstructorCaller.full(items,context,labels):ClosedNominalArrayConstructorCaller.shortCall(items,labels);
    if(route==2){try{return operation==0?Core.class.getConstructor(Item[][].class,Context.class,String[].class).newInstance(items,context,labels):Core.class.getConstructor(Item[][].class,String[].class).newInstance(items,labels);}catch(InvocationTargetException error){throw error.getCause();}}
    if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Item[][].class,Context.class,String[].class)).invokeExact(items,context,labels);
    return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Item[][].class,String[].class)).invokeExact(items,labels);
  }
  public static void main(String[] args)throws Throwable {
    final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
    require(Core.class.getConstructor(Item[][].class,Context.class,String[].class).isVarArgs());
    require(Core.class.getConstructor(Item[][].class,String[].class).isVarArgs());
    require(Core.class.getConstructor(String[].class).isVarArgs());
    Constructor<?> target=Core.class.getConstructor(java.util.List.class,String[].class,Item[][].class,Context.class);
    require(!target.isVarArgs()&&target.getTypeParameters().length==0);
    require(((ParameterizedType)target.getGenericParameterTypes()[0]).getActualTypeArguments()[0]==String.class);
    final Context context=new Context();final Item oldItem=new Item(),element=new Item();
    final Item[][] items=mode==1?null:mode==2?new Item[0][]:mode==3?new Child[][]{new Child[1]}:new Item[][]{new Item[]{oldItem}};
    final Item[] row=mode==4?new Child[1]:mode==13?null:mode==14?new Item[0]:new Item[]{oldItem};
    final String[] labels=mode==11?null:mode==12?new String[0]:new String[]{"before","tail"};
    final String changed=new String("owned label");final Object desired=mode==8?Integer.valueOf(9):mode==9?null:new String("owned result");
    final RuntimeException abrupt=new IllegalStateException("owned callback");final Error error=new AssertionError("owned callback error");
    Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==7?2:mode==10?7:mode==15?1:0;
    for(int attempt=0;attempt<2;attempt++) {
      Trace.events=0;Trace.seen=null;Trace.result=new Object();int calls=Trace.calls;
      final Object beforeResult=Trace.result;final Item[] beforeRow=items!=null&&items.length!=0?items[0]:null;
      final Item beforeElement=row!=null&&row.length!=0?row[0]:null;final String beforeLabel=labels!=null&&labels.length!=0?labels[0]:null;
      Trace.action=new Runnable(){public void run(){
        require(Trace.seen!=null&&Trace.seen.names==null&&Trace.seen.items==items&&Trace.seen.labels==labels&&Trace.seen.context==(operation==0?context:null)&&Trace.seen.value==null);
        Trace.hit(3);if(mode==5)throw abrupt;if(mode==6)throw error;
        items[0]=row;Trace.hit(4);items[0][0]=element;Trace.hit(5);labels[0]=changed;Trace.hit(6);Trace.result=desired;
      }};
      Core result=null;Throwable failure=null;int length=-1;
      try{result=call(route,operation,items,context,labels);length=((String)result.value).length();}catch(Throwable caught){failure=caught;}
      boolean body=mode!=15,callback=body&&mode!=7;
      boolean rowStored=callback&&mode!=1&&mode!=2&&mode!=3&&mode!=5&&mode!=6;
      boolean elementStored=rowStored&&mode!=4&&mode!=13&&mode!=14;
      boolean labelStored=elementStored&&mode!=11&&mode!=12;
      boolean completed=labelStored&&mode!=10;
      long events=attempt==0?1:0;if(body)events=events*10+2;if(callback)events=events*10+3;
      if(rowStored)events=events*10+4;if(elementStored)events=events*10+5;
      if(labelStored){events=events*10+6;events=events*10+7;}
      if(completed){events=events*10+8;if(operation==1)events=events*10+9;}
      if(mode==15&&attempt==1)events=0;
      require(Trace.events==events&&Trace.calls==calls+(body?1:0));
      require((Trace.seen!=null)==callback&&Trace.result==(labelStored?desired:beforeResult));
      if(items!=null&&items.length!=0)require(items[0]==(rowStored?row:beforeRow));
      if(row!=null&&row.length!=0)require(row[0]==(elementStored?element:beforeElement));
      if(labels!=null&&labels.length!=0)require(labels[0]==(labelStored?changed:beforeLabel)&&labels[1].equals("tail"));
      if(mode==15){if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);else require(failure instanceof NoClassDefFoundError);}
      else if(mode==1||mode==9||mode==11||mode==13)require(failure instanceof NullPointerException);
      else if(mode==2||mode==12||mode==14)require(failure instanceof ArrayIndexOutOfBoundsException);
      else if(mode==3||mode==4)require(failure instanceof ArrayStoreException);
      else if(mode==5)require(failure==abrupt);else if(mode==6)require(failure==error);
      else if(mode==7||mode==10)require(failure==Trace.failure);else if(mode==8)require(failure instanceof ClassCastException);
      else require(failure==null&&length==12);
      require((result!=null)==completed);
      if(completed)require(result==Trace.seen&&result.value==desired);
      System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
    }
  }
}
