import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class NestedArrayResultHarness {
  static void require(boolean value) { if(!value)throw new AssertionError("owned nested array result changed"); }
  static Object call(int route,int operation,Object[][] rows) throws Throwable {
    if(operation==2) {
      String[] first=(String[])rows[0],second=(String[])rows[1];
      if(route==0)return Client.packed(first,second);
      if(route==1)return NestedArrayResultCaller.packed(first,second);
      if(route==2) {
        try{return Client.class.getMethod("packed",String[].class,String[].class).invoke(null,first,second);}
        catch(InvocationTargetException error){throw error.getCause();}
      }
      return (String[])MethodHandles.publicLookup().findStatic(Client.class,"packed",
        MethodType.methodType(String[].class,String[].class,String[].class)).invokeExact(first,second);
    }
    boolean checked=operation==1;
    if(route==0)return checked?Client.checked(rows):Client.call(rows);
    if(route==1)return checked?NestedArrayResultCaller.checked(rows):NestedArrayResultCaller.call(rows);
    String name=checked?"checked":"call";
    if(route==2) {
      try{return Client.class.getMethod(name,Object[][].class).invoke(null,(Object)rows);}
      catch(InvocationTargetException error){throw error.getCause();}
    }
    if(checked)return (String[])MethodHandles.publicLookup().findStatic(Client.class,name,
      MethodType.methodType(String[].class,Object[][].class)).invokeExact(rows);
    return (Object[])MethodHandles.publicLookup().findStatic(Client.class,name,
      MethodType.methodType(Object[].class,Object[][].class)).invokeExact(rows);
  }
  public static void main(String[] args) throws Throwable {
    final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
    Method method=Class.forName("fixture.Provider",false,Client.class.getClassLoader()).getMethod("select",Object[][].class);
    TypeVariable<?>[] formals=method.getTypeParameters();
    require(Modifier.isStatic(method.getModifiers())&&method.isVarArgs()&&formals.length==1
      &&formals[0].getBounds().length==1&&formals[0].getBounds()[0]==Object.class);
    Type[] parameters=method.getGenericParameterTypes();require(parameters.length==1);
    GenericArrayType matrix=(GenericArrayType)parameters[0];
    require(matrix.getGenericComponentType() instanceof GenericArrayType
      &&((GenericArrayType)matrix.getGenericComponentType()).getGenericComponentType().equals(formals[0])
      &&((GenericArrayType)method.getGenericReturnType()).getGenericComponentType().equals(formals[0]));
    Trace.failAt=mode>=1&&mode<=5?mode:mode==17?6:mode==18?7:0;
    Trace.failure=new RuntimeException("owned stage failure");
    final Error abrupt=new AssertionError("owned callback failure");
    Object sentinel=new Object();Object[][] unseen={{sentinel}};
    String[] first={new String("owned first")},second={new String("owned second"),new String("owned tail")};
    Object[][] rows=mode==6?null:mode==7?new Object[0][]:mode==9?new String[][]{first,second}:new Object[][]{first,second};
    Trace.row=mode==8?new Object():mode==9?new Object[]{"owned row"}:mode==10?null:mode==11?new String[0]:new String[]{new String("owned row")};
    Trace.element=mode==12?new Object():new String("owned replacement");
    final String[] alternate={new String("owned callback result")};
    final Object initial=mode==13?new Object[]{new Object()}:mode==14?new Object():mode==15?null:mode==19?Trace.row:new String[]{new String("owned result")};
    Trace.action=new Action(){public void run(){Trace.hit(8);if(mode==16)throw abrupt;if(mode==20)Trace.result=alternate;}};
    for(int attempt=0;attempt<2;attempt++) {
      Trace.events=0;Trace.length=-1;Trace.seen=unseen;Trace.after=sentinel;Trace.result=initial;
      Object[] before=rows!=null&&rows.length!=0?rows[0]:null;
      Object prior=before!=null&&before.length!=0?before[0]:null;
      Object rowPrior=Trace.row instanceof Object[]&&((Object[])Trace.row).length!=0?((Object[])Trace.row)[0]:null;
      Throwable error=null;Object result=null;
      try{result=call(route,operation,rows);}catch(Throwable problem){error=problem;}
      int trace=attempt==0?1:0;
      boolean body=mode!=1;if(body)trace=trace*10+2;
      boolean length=body&&mode!=2&&mode!=6;if(length)trace=trace*10+3;
      boolean rowStored=length&&mode!=3&&mode!=7&&mode!=8&&mode!=9;if(rowStored)trace=trace*10+4;
      boolean elementStored=rowStored&&mode!=4&&mode!=10&&mode!=11&&mode!=12;if(elementStored)trace=trace*10+5;
      boolean callback=elementStored&&mode!=5;if(callback)trace=trace*10+8;
      boolean postlude=callback&&mode!=16&&mode!=14;if(postlude)trace=trace*10+6;
      boolean caller=postlude&&mode!=17&&!(operation!=0&&mode==13);if(caller)trace=trace*10+7;
      require(Trace.events==trace&&Trace.length==(length?(operation==2?2:rows.length):-1));
      if(!body)require(Trace.seen==unseen);
      else if(operation!=2)require(Trace.seen==rows);
      else require(Trace.seen!=rows&&Trace.seen.getClass()==String[][].class&&Trace.seen.length==2&&Trace.seen[1]==second);
      if(body&&Trace.seen!=null&&Trace.seen.length!=0)require(Trace.seen[0]==(rowStored?Trace.row:before));
      if(rowStored&&Trace.row instanceof Object[]&&((Object[])Trace.row).length!=0)
        require(((Object[])Trace.row)[0]==(elementStored?Trace.element:rowPrior));
      if(!rowStored&&before!=null&&before.length!=0)require(before[0]==prior);
      if(operation==2)require(rows[0]==before&&rows[1]==second);
      require(Trace.after==(caller?Trace.result:sentinel)&&Trace.result==(mode==20&&callback?alternate:initial));
      if(mode==1) {
        if(attempt==0)require(error instanceof ExceptionInInitializerError&&error.getCause()==Trace.failure);
        else require(error instanceof NoClassDefFoundError);
      }else if(mode>=2&&mode<=5||mode==17||mode==18)require(error==Trace.failure);
      else if(mode==6||mode==10)require(error instanceof NullPointerException);
      else if(mode==7||mode==11)require(error instanceof ArrayIndexOutOfBoundsException);
      else if(mode==8||mode==14||operation!=0&&mode==13)require(error instanceof ClassCastException);
      else if(mode==9||mode==12)require(error instanceof ArrayStoreException);
      else if(mode==16)require(error==abrupt);
      else require(error==null&&result==Trace.result);
      if(error==null&&mode==19)require(result==Trace.seen[0]);
      System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
    }
  }
}
