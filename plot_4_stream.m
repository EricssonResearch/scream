function plot_4_stream(a,Tlim,maxR,maxD,maxRT)
  % Plot stdout for 4-streaming 
  % script is adapted to 4 streams
  % Tlim [min max] time e.g [0 1000]
  % maxR max rate [Mbps]
  % maxD max delay [s]
  % example:
  % >a = load('logfile.txt');
  % >plot_vay_4(a,[0 1000],15,0.1);
  
  T=a(:,1);T=T-T(1);
  
  K=7;L=1;
  subplot(K,1,L);L=L+1;
  plot(T,a(:,4)/1e3,T,a(:,5)/1e3,':',T,a(:,7)*max(a(:,4))/1e3*0.05,':k');
  set(gca,'FontSize',12);grid on;
  title('CWND and bytes in flight [kbyte]')
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
  plot(T,a(:,8),T,a(:,8+6),T,a(:,8+12),T,a(:,8+18));
  set(gca,'FontSize',12);grid on;
  set(gca,'XTickLabel',[]);  
  legend('0','1','2','3')
  title('RTP queue delay[s]')
  ylim([0 maxD]);
  xlim(Tlim);

  subplot(K,1,L);L=L+1;
  ce = a(:,13)+a(:,13+6)+a(:,13+12)+a(:,13+18);
  loss = a(:,12)+a(:,12+6)+a(:,12+12)+a(:,12+18);
  plot(T,ce/1e2,T,loss/1e2,T,a(:,6)/1e3,'k');
  set(gca,'FontSize',12);grid on;
  set(gca,'XTickLabel',[]);  
  title('Tx rate [Mbps]')
  legend('ce*10','loss*10','Total')
  ylim([0 maxRT]);
  xlim(Tlim);

  subplot(K,1,L);L=L+1;
  plot(T,a(:,11)/1e3,T,a(:,11+6)/1e3,T,a(:,11+12)/1e3,T,a(:,11+18)/1e3);
  set(gca,'FontSize',12);grid on;
  set(gca,'XTickLabel',[]);  
  title('Tx rate streams [Mbps]')
  legend('0','1','2','3')
  ylim([0 maxR]);
  xlim(Tlim);

  subplot(K,1,L);L=L+1;
  plot(T,a(:,9)/1e3,'-',T,a(:,9+6)/1e3,'d-',T,a(:,9+12)/1e3,'o-',T,a(:,9+18)/1e3,'*-');
  set(gca,'FontSize',12);grid on;
  set(gca,'XTickLabel',[]);  
  legend('0','1','2','3')
  xlim(Tlim);
  ylim([0 maxR]);
  title('Target rate [Mbps]')
  
  subplot(K,1,L);L=L+1;
  plot(T,a(:,10)/1e3,T,a(:,10+6)/1e3,T,a(:,10+12)/1e3,T,a(:,10+18)/1e3);
  set(gca,'FontSize',12);grid on;
  legend('0','1','2','3')
  title('Encoder rate [Mbps]')
  xlim(Tlim);
  ylim([0 maxR]);
  xlabel('T [s]')
  
end