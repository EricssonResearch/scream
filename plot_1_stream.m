function plot_1_stream(a,Tlim,maxR,maxD,maxRT, U)
  % Plot stdout for 1-streaming 
  % script is adapted to 1 stream
  % Tlim [min max] time e.g [0 1000]
  % maxR max rate [Mbps]
  % maxD max delay [s]
  % example:
  % >a = load('logfile.txt');
  % >plot_vay_4(a,[0 1000],15,0.1);
  
  T=a(:,1);T=T-T(1);
  
  B = ones(1,U)/U;
  K=6;L=1;
  subplot(K,1,L);L=L+1;
  %plot(T,a(:,4)/1e3,T,a(:,5)/1e3,':',T,a(:,7)*max(a(:,4))/1e3*0.05,':k');
  plot(T,a(:,4)/1200,T,a(:,5)/1200,':');
  set(gca,'FontSize',12);grid on;
  %title('CWND and bytes in flight [kbyte]')
  title('CWND and bytes in flight [MSS]')
  set(gca,'XTickLabel',[]);  
  xlim(Tlim);

  subplot(K,1,L);L=L+1;
  plot(T,a(:,2),T,a(:,3));
  set(gca,'FontSize',12);grid on;
  set(gca,'XTickLabel',[]);  
  title('Est. queue delay and RTT [s]')
  ylim([0 maxD]);
  xlim(Tlim);

  subplot(K,1,L);L=L+1;
  plot(T,a(:,8));
  set(gca,'FontSize',12);grid on;
  set(gca,'XTickLabel',[]);  
  title('RTP queue delay[s]')
  ylim([0 maxD]);
  xlim(Tlim);

  subplot(K,1,L);L=L+1;
  ce = a(:,13);
  loss = a(:,12);
  plot(T,ce/1e2,T,loss/1e2,T,filter(B,1,a(:,6))/1e3,'k');
  set(gca,'FontSize',12);grid on;
  set(gca,'XTickLabel',[]);  
  title('Tx rate [Mbps]')
  legend('ce*10','loss*10','Total')
  ylim([0 maxRT]);
  xlim(Tlim);

  subplot(K,1,L);L=L+1;
  plot(T,a(:,9)/1e3);
  set(gca,'FontSize',12);grid on;
  set(gca,'XTickLabel',[]);  
  xlim(Tlim);
  ylim([0 maxR]);
  title('Target rate [Mbps]')
  
  subplot(K,1,L);L=L+1;
  plot(T,filter(B,1,a(:,10))/1e3);
  set(gca,'FontSize',12);grid on;
  title('Encoder rate [Mbps]')
  xlim(Tlim);
  ylim([0 maxR]);
  xlabel('T [s]')
  
end